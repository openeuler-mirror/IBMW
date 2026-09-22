// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace ibmw::cli::topic {

class BandwidthStatistics {
 public:
  using Clock = std::chrono::steady_clock;
  struct Result {
    uint64_t received = 0;
    size_t window = 0;
    bool ready = false;
    double bytes_per_second = 0;
    double mean = 0;
    size_t minimum = 0;
    size_t maximum = 0;
  };

  explicit BandwidthStatistics(size_t window) : window_(window) {
    if (window < 2) throw std::invalid_argument("--window must be at least 2.");
  }

  void Observe(size_t bytes) {
    std::lock_guard lock(mutex_);
    ObserveLocked(bytes, Clock::now());
  }
  void Observe(size_t bytes, Clock::time_point time) {
    std::lock_guard lock(mutex_);
    ObserveLocked(bytes, time);
  }

  Result Snapshot() const {
    std::lock_guard lock(mutex_);
    return SnapshotLocked(Clock::now());
  }
  Result Snapshot(Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    return SnapshotLocked(now);
  }

 private:
  struct Sample {
    size_t bytes;
    Clock::time_point time;
  };

  void ObserveLocked(size_t bytes, Clock::time_point time) {
    ++received_;
    if (!samples_.empty() && time < samples_.back().time) samples_.clear();
    samples_.push_back({bytes, time});
    if (samples_.size() > window_) samples_.pop_front();
  }

  Result SnapshotLocked(Clock::time_point now) const {
    Result result{.received = received_, .window = samples_.size()};
    if (samples_.empty()) return result;
    // Accumulate without overflowing an integer byte counter for large windows.
    long double total = 0;
    result.minimum = samples_.front().bytes;
    for (const auto& sample : samples_) {
      total += sample.bytes;
      result.minimum = std::min(result.minimum, sample.bytes);
      result.maximum = std::max(result.maximum, sample.bytes);
    }
    result.mean = static_cast<double>(total / samples_.size());
    const auto elapsed = std::chrono::duration<double>(now - samples_.front().time).count();
    result.ready = samples_.size() >= 2 && elapsed > 0 && now >= samples_.back().time;
    // Include silence since the last message, as ros2 topic bw does.
    if (result.ready) result.bytes_per_second = static_cast<double>(total / elapsed);
    return result;
  }

  const size_t window_;
  mutable std::mutex mutex_;
  uint64_t received_ = 0;
  std::deque<Sample> samples_;
};

}  // namespace ibmw::cli::topic
