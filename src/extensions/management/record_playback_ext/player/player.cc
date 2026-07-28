// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/player/player.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <set>
#include <thread>
#include <unordered_map>

#include <unistd.h>

#include "base/exception.h"
#include "base/time_util.h"
#include "record_playback_ext/discovery/bag_topic_graph.h"
#include "record_playback_ext/runtime/runtime_playback_stream.h"
#include "record_playback_ext/runtime/runtime_registry.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

using SteadyClock = std::chrono::steady_clock;

std::string TopicKey(std::string_view topic, std::string_view msg_type) {
  return std::string(topic) + '\0' + std::string(msg_type);
}

std::string PlaybackSessionId(uint64_t loop_index) {
  return "playback-" + std::to_string(::getpid()) + "-" +
         std::to_string(ibmw::common::util::GetCurTimestampNs()) + "-" +
         std::to_string(loop_index);
}

bool IsLocalOnly(const TopicAdvertisement& advertisement) {
  return advertisement.topic.available_drivers.empty() ||
         std::all_of(
             advertisement.topic.available_drivers.begin(),
             advertisement.topic.available_drivers.end(),
             [](std::string_view driver) { return driver == "local"; });
}

struct Route {
  TopicAdvertisement advertisement;
  std::unique_ptr<UnixRuntimePlaybackClient> client;
  bool direct = false;
};

using RouteMap = std::unordered_map<std::string, std::vector<Route>>;

void StopRoutes(RouteMap& routes) {
  std::exception_ptr error;
  for (auto& [key, entries] : routes) {
    static_cast<void>(key);
    for (auto& entry : entries) {
      if (entry.client) {
        try {
          entry.client->Stop();
        } catch (...) {
          if (!error) {
            error = std::current_exception();
          }
        }
      }
    }
  }
  routes.clear();
  if (error) {
    std::rethrow_exception(error);
  }
}

std::vector<TopicAdvertisement> SelectAdvertisements(
    const std::vector<TopicAdvertisement>& advertisements,
    const BagChannelInfo& channel,
    const BagPlayer::RouteCompatibilityCallback& compatibility_callback) {
  std::vector<TopicAdvertisement> matching;
  for (const auto& advertisement : advertisements) {
    if (advertisement.published ||
        advertisement.topic.msg_type != channel.topic.msg_type) {
      continue;
    }
    const bool exact =
        advertisement.topic.serialization_type ==
            channel.topic.serialization_type &&
        advertisement.topic.schema.schema_hash ==
            channel.topic.schema.schema_hash;
    if (!exact &&
        (!compatibility_callback ||
         !compatibility_callback(channel, advertisement))) {
      continue;
    }
    matching.emplace_back(advertisement);
  }
  if (matching.empty()) {
    return {};
  }

  std::vector<TopicAdvertisement> result;
  const auto shared = std::find_if(
      matching.begin(), matching.end(),
      [](const auto& advertisement) { return !IsLocalOnly(advertisement); });
  if (shared != matching.end()) {
    result.emplace_back(*shared);
  }
  std::set<std::string> runtimes;
  for (const auto& advertisement : result) {
    runtimes.emplace(advertisement.runtime_id);
  }
  for (const auto& advertisement : matching) {
    if (IsLocalOnly(advertisement) &&
        runtimes.emplace(advertisement.runtime_id).second) {
      result.emplace_back(advertisement);
    }
  }
  return result;
}

bool InterruptibleDelay(PlayerControl& control, double seconds) {
  if (seconds <= 0.0) {
    return !control.IsStopped();
  }
  const auto deadline = SteadyClock::now() +
                        std::chrono::duration_cast<SteadyClock::duration>(
                            std::chrono::duration<double>(seconds));
  while (!control.IsStopped() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return !control.IsStopped();
}

}  // namespace

void PlayerControl::TogglePause() {
  {
    std::lock_guard lock(mutex_);
    paused_ = !paused_;
  }
  cv_.notify_all();
}

void PlayerControl::Stop() {
  stopped_ = true;
  cv_.notify_all();
}

bool PlayerControl::IsPaused() const {
  std::lock_guard lock(mutex_);
  return paused_;
}

BagPlayer::BagPlayer(
    const BagReader& reader,
    PlayerOptions options,
    PlayerControl& control,
    EventCallback event_callback,
    DirectSupportCallback direct_support_callback,
    DirectPublishCallback direct_publish_callback,
    RouteCompatibilityCallback route_compatibility_callback,
    TranscodeCallback transcode_callback)
    : reader_(reader),
      options_(std::move(options)),
      control_(control),
      event_callback_(std::move(event_callback)),
      direct_support_callback_(std::move(direct_support_callback)),
      direct_publish_callback_(std::move(direct_publish_callback)),
      route_compatibility_callback_(
          std::move(route_compatibility_callback)),
      transcode_callback_(std::move(transcode_callback)) {
  MW_ASSERT(options_.read_ahead_queue_size > 0, "Read-ahead queue cannot be zero.");
  MW_ASSERT(options_.rate > 0.0, "Playback rate must be greater than zero.");
  MW_ASSERT(options_.delay_seconds >= 0.0, "Playback delay cannot be negative.");
  MW_ASSERT(
      options_.start_offset_seconds >= 0.0,
      "Playback start offset cannot be negative.");
}

uint64_t BagPlayer::Play() {
  uint64_t published = 0;
  uint64_t loop_index = 0;
  do {
    if (!InterruptibleDelay(control_, options_.delay_seconds)) {
      break;
    }
    published += PlayOnce(loop_index++);
  } while (options_.loop && !control_.IsStopped());
  return published;
}

uint64_t BagPlayer::PlayOnce(uint64_t loop_index) {
  const auto& info = reader_.Info();
  const std::set<std::string> selected_topics(
      options_.topics.begin(), options_.topics.end());
  std::unordered_map<std::string, BagChannelInfo> route_channels;
  for (auto channel : info.channels) {
    if (!selected_topics.empty() &&
        !selected_topics.contains(channel.topic.topic)) {
      continue;
    }
    const auto remap = options_.remaps.find(channel.topic.topic);
    if (remap != options_.remaps.end()) {
      channel.topic.topic = remap->second;
    }
    route_channels.try_emplace(
        TopicKey(channel.topic.topic, channel.topic.msg_type),
        std::move(channel));
  }
  MW_ASSERT(!route_channels.empty(), "No bag topics match the playback selection.");

  RouteMap routes;
  RuntimeRegistry registry;
  bool waiting_printed = false;
  while (!control_.IsStopped()) {
    BagTopicGraph graph;
    graph.ReplaceRuntimes(registry.DiscoverLocalRuntimes());
    bool complete = true;
    RouteMap candidate_routes;
    const auto add_routes =
        [&](const std::string& key,
            std::vector<TopicAdvertisement> advertisements) {
          auto& entries = candidate_routes[key];
          for (auto advertisement : advertisements) {
            if (!IsLocalOnly(advertisement) && direct_support_callback_ &&
                direct_support_callback_(advertisement)) {
              entries.emplace_back(Route{
                  .advertisement = std::move(advertisement),
                  .direct = true});
              continue;
            }
            auto client = std::make_unique<UnixRuntimePlaybackClient>(
                advertisement.control_socket);
            try {
              client->Start(advertisement);
            } catch (...) {
              return false;
            }
            entries.emplace_back(Route{
                .advertisement = std::move(advertisement),
                .client = std::move(client),
                .direct = false});
          }
          return true;
        };
    for (const auto& [key, channel] : route_channels) {
      auto advertisements =
          SelectAdvertisements(
              graph.Find(channel.topic.topic), channel,
              route_compatibility_callback_);
      if (advertisements.empty() ||
          !add_routes(key, std::move(advertisements))) {
        complete = false;
        break;
      }
    }
    if (complete) {
      routes = std::move(candidate_routes);
      break;
    }
    StopRoutes(candidate_routes);
    if (!waiting_printed && event_callback_) {
      event_callback_("Waiting for compatible subscriber Runtime(s)...");
      waiting_printed = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (control_.IsStopped()) {
    StopRoutes(routes);
    return 0;
  }
  if (event_callback_) {
    for (const auto& [key, entries] : routes) {
      static_cast<void>(key);
      for (const auto& entry : entries) {
        event_callback_(
            "  " + entry.advertisement.topic.topic + " -> Runtime " +
            entry.advertisement.runtime_id +
            (entry.direct ? " direct serialized publisher"
                          : " embedded publisher"));
      }
    }
  }

  struct QueueState {
    std::mutex mutex;
    std::condition_variable can_push;
    std::condition_variable can_pop;
    std::deque<BagMessage> messages;
    bool finished = false;
    bool stopped = false;
    std::exception_ptr error;
  } queue;

  const uint64_t offset_ns = static_cast<uint64_t>(
      options_.start_offset_seconds * 1000000000.0);
  const uint64_t playback_start_timestamp = info.start_timestamp + offset_ns;
  BagReadOptions read_options{
      .start_timestamp = playback_start_timestamp,
      .topics = options_.topics};
  std::thread producer([&] {
    try {
      reader_.ReadMessages(read_options, [&](BagMessage&& message) {
        std::unique_lock lock(queue.mutex);
        queue.can_push.wait(lock, [&] {
          return queue.stopped || control_.IsStopped() ||
                 queue.messages.size() < options_.read_ahead_queue_size;
        });
        if (queue.stopped || control_.IsStopped()) {
          return false;
        }
        queue.messages.emplace_back(std::move(message));
        queue.can_pop.notify_one();
        return true;
      });
    } catch (...) {
      std::lock_guard lock(queue.mutex);
      queue.error = std::current_exception();
    }
    {
      std::lock_guard lock(queue.mutex);
      queue.finished = true;
    }
    queue.can_pop.notify_all();
  });

  const std::string session_id = PlaybackSessionId(loop_index);
  auto wall_origin = SteadyClock::now();
  auto pause_started = SteadyClock::time_point{};
  uint64_t published = 0;
  std::exception_ptr playback_error;
  const auto publish_to_route =
      [&](const std::vector<Route>& route,
          const ibmw::transport::SerializedMessage& output) {
        for (size_t index = 0; index < route.size(); ++index) {
          auto copy = output;
          if (index + 1 != route.size()) {
            copy.payload =
                ibmw::transport::CopySerializedPayload(
                    *output.payload);
          }
          const auto& target = route[index].advertisement;
          const bool exact =
              copy.topic.serialization_type ==
                  target.topic.serialization_type &&
              copy.topic.schema.schema_hash ==
                  target.topic.schema.schema_hash;
          if (!exact) {
            MW_ASSERT(
                transcode_callback_,
                "No playback transcoder for topic '{}'.",
                target.topic.topic);
            copy = transcode_callback_(target, copy);
          } else {
            copy.topic = target.topic;
          }
          if (route[index].direct) {
            MW_ASSERT(
                direct_publish_callback_,
                "Direct playback publisher callback is unavailable.");
            direct_publish_callback_(target, copy);
          } else {
            route[index].client->Publish(copy);
          }
        }
      };
  try {
    while (!control_.IsStopped()) {
      BagMessage message;
      {
        std::unique_lock lock(queue.mutex);
        queue.can_pop.wait(lock, [&] {
          return control_.IsStopped() || !queue.messages.empty() ||
                 queue.finished;
        });
        if (control_.IsStopped() ||
            (queue.messages.empty() && queue.finished)) {
          break;
        }
        message = std::move(queue.messages.front());
        queue.messages.pop_front();
        queue.can_push.notify_one();
      }

      const auto relative_ns = static_cast<uint64_t>(
          static_cast<double>(message.log_time - playback_start_timestamp) /
          options_.rate);
      while (!control_.IsStopped()) {
        std::unique_lock control_lock(control_.mutex_);
        if (control_.paused_) {
          if (pause_started == SteadyClock::time_point{}) {
            pause_started = SteadyClock::now();
          }
          control_.cv_.wait_for(control_lock, std::chrono::milliseconds(50));
          continue;
        }
        if (pause_started != SteadyClock::time_point{}) {
          wall_origin += SteadyClock::now() - pause_started;
          pause_started = {};
        }
        const auto message_target =
            wall_origin + std::chrono::nanoseconds(relative_ns);
        if (SteadyClock::now() < message_target) {
          control_.cv_.wait_until(control_lock, message_target);
          continue;
        }
        break;
      }
      if (control_.IsStopped()) {
        break;
      }

      const auto remap = options_.remaps.find(message.channel.topic.topic);
      if (remap != options_.remaps.end()) {
        message.channel.topic.topic = remap->second;
      }
      const auto route = routes.find(TopicKey(
          message.channel.topic.topic, message.channel.topic.msg_type));
      MW_ASSERT(
          route != routes.end(), "No playback route for topic '{}'.",
          message.channel.topic.topic);
      ibmw::transport::SerializedMessage output{
          .topic = message.channel.topic,
          .identity =
              {.source_id = session_id,
               .publisher_id = "channel-" +
                               std::to_string(message.channel.id),
               .sequence = message.sequence},
          .publish_timestamp = message.publish_time,
          .receive_timestamp = message.log_time,
          .source_driver = "runtime-playback",
          .payload = std::move(message.payload)};
      publish_to_route(route->second, output);
      ++published;
    }
  } catch (...) {
    playback_error = std::current_exception();
  }

  {
    std::lock_guard lock(queue.mutex);
    queue.stopped = true;
  }
  queue.can_push.notify_all();
  queue.can_pop.notify_all();
  if (producer.joinable()) {
    producer.join();
  }
  std::exception_ptr route_stop_error;
  try {
    StopRoutes(routes);
  } catch (...) {
    route_stop_error = std::current_exception();
  }
  if (playback_error) {
    std::rethrow_exception(playback_error);
  }
  if (queue.error) {
    std::rethrow_exception(queue.error);
  }
  if (route_stop_error) {
    std::rethrow_exception(route_stop_error);
  }
  return published;
}

}  // namespace ibmw::extensions::record_playback_extension
