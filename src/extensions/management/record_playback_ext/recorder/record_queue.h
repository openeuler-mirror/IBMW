// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "engine/transport/serialized_message.h"

namespace ibmw::extensions::record_playback_extension {

class RecordQueue {
 public:
  enum class FullPolicy {
    kBlock,
    kDropNewest,
  };

  enum class PushResult {
    kPushed,
    kDropped,
    kClosed,
    kInvalid,
  };

  struct Options {
    size_t max_messages = 1024;
    size_t max_bytes = 100 * 1024 * 1024;
    // Blocking is retained only for explicit offline callers. Transport
    // ingress must select kDropNewest so callbacks never wait for storage.
    FullPolicy full_policy = FullPolicy::kDropNewest;
  };

  struct Statistics {
    uint64_t pushed = 0;
    uint64_t popped = 0;
    uint64_t dropped = 0;
    size_t queued_messages = 0;
    size_t queued_bytes = 0;
    bool closed = false;
  };

  explicit RecordQueue(Options options);
  ~RecordQueue() = default;

  RecordQueue(const RecordQueue&) = delete;
  RecordQueue& operator=(const RecordQueue&) = delete;

  PushResult Push(
      ibmw::runtime::core::transport::SerializedMessage message);
  std::optional<ibmw::runtime::core::transport::SerializedMessage> Pop();
  std::optional<ibmw::runtime::core::transport::SerializedMessage> TryPop();

  void Close();
  Statistics GetStatistics() const;

 private:
  bool FitsLocked(size_t payload_size) const noexcept;
  std::optional<ibmw::runtime::core::transport::SerializedMessage>
  PopLocked();

  const Options options_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_cv_;
  std::condition_variable not_full_cv_;
  std::deque<ibmw::runtime::core::transport::SerializedMessage> queue_;
  size_t queued_bytes_ = 0;
  uint64_t pushed_ = 0;
  uint64_t popped_ = 0;
  uint64_t dropped_ = 0;
  bool closed_ = false;
};

}  // namespace ibmw::extensions::record_playback_extension
