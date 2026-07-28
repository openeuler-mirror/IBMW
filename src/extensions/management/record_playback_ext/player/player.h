// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cpp/transport/serialized_message.h"
#include "record_playback_ext/discovery/topic_advertisement.h"
#include "record_playback_ext/storage/mcap_bag_reader.h"

namespace ibmw::extensions::record_playback_extension {

struct PlayerOptions {
  size_t read_ahead_queue_size = 1000;
  double rate = 1.0;
  std::vector<std::string> topics;
  bool loop = false;
  std::map<std::string, std::string> remaps;
  double delay_seconds = 0.0;
  bool start_paused = false;
  double start_offset_seconds = 0.0;
};

class PlayerControl {
 public:
  explicit PlayerControl(bool start_paused) : paused_(start_paused) {}

  void TogglePause();
  void Stop();
  bool IsPaused() const;
  bool IsStopped() const noexcept { return stopped_.load(); }

 private:
  friend class BagPlayer;

  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool paused_ = false;
};

class BagPlayer {
 public:
  using EventCallback = std::function<void(std::string_view message)>;
  using DirectSupportCallback =
      std::function<bool(const TopicAdvertisement& advertisement)>;
  using DirectPublishCallback = std::function<void(
      const TopicAdvertisement& advertisement,
      const ibmw::transport::SerializedMessage& message)>;
  using RouteCompatibilityCallback = std::function<bool(
      const BagChannelInfo& channel,
      const TopicAdvertisement& advertisement)>;
  using TranscodeCallback = std::function<
      ibmw::transport::SerializedMessage(
          const TopicAdvertisement& advertisement,
          const ibmw::transport::SerializedMessage& message)>;

  BagPlayer(
      const McapBagReader& reader,
      PlayerOptions options,
      PlayerControl& control,
      EventCallback event_callback = {},
      DirectSupportCallback direct_support_callback = {},
      DirectPublishCallback direct_publish_callback = {},
      RouteCompatibilityCallback route_compatibility_callback = {},
      TranscodeCallback transcode_callback = {});

  uint64_t Play();

 private:
  uint64_t PlayOnce(uint64_t loop_index);

  const McapBagReader& reader_;
  PlayerOptions options_;
  PlayerControl& control_;
  EventCallback event_callback_;
  DirectSupportCallback direct_support_callback_;
  DirectPublishCallback direct_publish_callback_;
  RouteCompatibilityCallback route_compatibility_callback_;
  TranscodeCallback transcode_callback_;
};

}  // namespace ibmw::extensions::record_playback_extension
