// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/bag_capture_planner.h"

#include <algorithm>
#include <exception>
#include <set>
#include <tuple>
#include <utility>

#include "base/exception.h"
#include "engine/transport/serialized_transport_driver.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

constexpr int kRuntimeTapCapturePriority = 5;

bool SameEndpoint(
    const TopicAdvertisement& lhs,
    const TopicAdvertisement& rhs) {
  return lhs.runtime_id == rhs.runtime_id &&
         lhs.publisher_id == rhs.publisher_id &&
         lhs.control_socket == rhs.control_socket &&
         lhs.topic.msg_type == rhs.topic.msg_type &&
         lhs.topic.serialization_type == rhs.topic.serialization_type &&
         lhs.topic.schema.schema_hash == rhs.topic.schema.schema_hash;
}

}  // namespace

CapturePlan::~CapturePlan() {
  try {
    StopAll();
  } catch (...) {
  }
}

void CapturePlan::Add(CapturePlanEntry entry) {
  MW_ASSERT(started_count_ == 0, "Cannot modify a started CapturePlan.");
  MW_ASSERT(entry.source != nullptr, "CapturePlan source cannot be null.");
  entries_.emplace_back(std::move(entry));
}

void CapturePlan::StartAll(
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(started_count_ == 0, "CapturePlan is already started.");
  try {
    for (auto& entry : entries_) {
      entry.source->Start(callback);
      ++started_count_;
    }
  } catch (...) {
    try {
      StopAll();
    } catch (...) {
    }
    throw;
  }
}

void CapturePlan::StopAll() {
  std::exception_ptr first_error;
  while (started_count_ > 0) {
    --started_count_;
    try {
      entries_[started_count_].source->Stop();
    } catch (...) {
      if (!first_error) {
        first_error = std::current_exception();
      }
    }
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

void BagCapturePlanner::RegisterTransportSourceFactory(
    std::string driver_name,
    TransportSourceFactory factory) {
  MW_ASSERT(!driver_name.empty(), "Driver name cannot be empty.");
  MW_ASSERT(factory, "Transport source factory cannot be empty.");
  transport_factories_[std::move(driver_name)] = std::move(factory);
}

void BagCapturePlanner::SetRuntimeTapSourceFactory(
    RuntimeTapSourceFactory factory) {
  MW_ASSERT(factory, "Runtime tap source factory cannot be empty.");
  runtime_tap_factory_ = std::move(factory);
}

CapturePlan BagCapturePlanner::BuildPlan(
    const std::vector<std::string>& topics) const {
  MW_ASSERT(!topics.empty(), "At least one topic is required.");
  CapturePlan plan;
  std::set<std::string> unique_topics;

  for (const auto& topic_name : topics) {
    MW_ASSERT(!topic_name.empty(), "Topic name cannot be empty.");
    if (!unique_topics.emplace(topic_name).second) {
      continue;
    }
    auto candidates = CandidatesFor(topic_name);
    MW_ASSERT(
        !candidates.empty(), "No capture source is available for topic '{}'.",
        topic_name);
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
      return std::tie(
                 lhs.priority, lhs.source_name, lhs.advertisement.runtime_id,
                 lhs.advertisement.publisher_id) <
             std::tie(
                 rhs.priority, rhs.source_name, rhs.advertisement.runtime_id,
                 rhs.advertisement.publisher_id);
    });

    const auto advertisements = graph_.Find(topic_name);
    const size_t published_endpoint_count = static_cast<size_t>(std::count_if(
        advertisements.begin(), advertisements.end(),
        [](const auto& advertisement) { return advertisement.published; }));
    const auto route_covers_publishers = [&](const Candidate& selected) {
      // A cached typed subscription receives every publisher on the Topic,
      // but its callback cannot recover a stable native publisher identity.
      // Use it only when the graph contains one published endpoint; otherwise
      // use native serialized capture or one Runtime Tap per publisher.
      if (selected.capture_mode == "cached_serialized" &&
          published_endpoint_count != 1) {
        return false;
      }
      return std::all_of(
          advertisements.begin(), advertisements.end(),
          [&](const auto& advertisement) {
            if (!advertisement.published) {
              return true;
            }
            return std::any_of(
                candidates.begin(), candidates.end(), [&](const auto& candidate) {
                  return candidate.source_name == selected.source_name &&
                         SameEndpoint(candidate.advertisement, advertisement);
                });
          });
    };
    const auto selected = std::find_if(
        candidates.begin(), candidates.end(), route_covers_publishers);
    MW_ASSERT(
        selected != candidates.end(),
        "No capture route covers every publisher for topic '{}'.", topic_name);

    const auto add_candidate = [&](const Candidate& candidate) {
      auto source = candidate.create();
      MW_ASSERT(
          source != nullptr, "Capture source factory returned null for '{}'.",
          topic_name);
      const std::string description = source->Description();
      plan.Add(CapturePlanEntry{
          .advertisement = candidate.advertisement,
          .source_name = candidate.source_name,
          .capture_mode = candidate.capture_mode,
          .description = description,
          .source = std::move(source)});
    };
    if (selected->source_name == "runtime-tap") {
      for (const auto& candidate : candidates) {
        if (candidate.source_name == selected->source_name) {
          add_candidate(candidate);
        }
      }
    } else {
      add_candidate(*selected);
    }
  }
  return plan;
}

std::vector<BagCapturePlanner::Candidate> BagCapturePlanner::CandidatesFor(
    std::string_view topic_name) const {
  std::vector<Candidate> result;
  const auto advertisements = graph_.Find(topic_name);
  MW_ASSERT(!advertisements.empty(), "Topic '{}' was not discovered.", topic_name);

  std::set<std::tuple<std::string, std::string, std::string>> descriptors;
  for (const auto& advertisement : advertisements) {
    descriptors.emplace(
        advertisement.topic.msg_type,
        advertisement.topic.serialization_type,
        advertisement.topic.schema.schema_hash);
  }
  MW_ASSERT(
      descriptors.size() == 1,
      "Topic '{}' has conflicting type or serialization advertisements.",
      topic_name);

  for (const auto& advertisement : advertisements) {
    for (const auto& driver : advertisement.topic.available_drivers) {
      const auto capability =
          advertisement.capture_capabilities.find(driver);
      if (capability == advertisement.capture_capabilities.end() ||
          capability->second.abi_version !=
              ibmw::runtime::core::transport::
                  SerializedTransportCapabilityV1::kAbiVersion ||
          (capability->second.mode != "native_serialized" &&
           capability->second.mode != "cached_serialized")) {
        continue;
      }
      const auto factory = transport_factories_.find(driver);
      if (factory == transport_factories_.end()) {
        continue;
      }
      const auto factory_copy = factory->second;
      result.emplace_back(Candidate{
          .priority = static_cast<int>(capability->second.priority),
          .source_name = driver,
          .capture_mode = capability->second.mode,
          .advertisement = advertisement,
          .create = [factory_copy, advertisement, driver] {
            return factory_copy(advertisement, driver);
          }});
    }
    if (advertisement.published && advertisement.embedded_tap &&
        runtime_tap_factory_) {
      const auto factory_copy = runtime_tap_factory_;
      result.emplace_back(Candidate{
          .priority = kRuntimeTapCapturePriority,
          .source_name = "runtime-tap",
          .capture_mode = "runtime_tap",
          .advertisement = advertisement,
          .create = [factory_copy, advertisement] {
            return factory_copy(advertisement);
          }});
    }
  }
  return result;
}

}  // namespace ibmw::extensions::record_playback_extension
