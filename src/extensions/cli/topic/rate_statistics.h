// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ibmw::cli::topic {

class RateStatistics {
 public:
  using Clock = std::chrono::steady_clock;
  struct Result {
    uint64_t received = 0;
    size_t window = 0;
    double rate = 0;
    double minimum = 0;
    double maximum = 0;
    double deviation = 0;
  };

  explicit RateStatistics(size_t window) : window_(window) {
    if (window < 2) throw std::invalid_argument("--window must be at least 2.");
  }

  void Observe() {
    std::lock_guard lock(mutex_);
    ObserveLocked(Clock::now());
  }
  void Observe(Clock::time_point time) {
    std::lock_guard lock(mutex_);
    ObserveLocked(time);
  }

  Result Snapshot() const {
    Result result;
    std::vector<double> intervals;
    {
      std::lock_guard lock(mutex_);
      result.received = received_;
      result.window = intervals_.size() + (last_ ? 1 : 0);
      intervals.assign(intervals_.begin(), intervals_.end());
    }
    if (intervals.empty()) return result;
    // Welford avoids cancellation when the frequency is high and jitter small.
    double mean = 0, m2 = 0;
    size_t count = 0;
    result.minimum = *std::min_element(intervals.begin(), intervals.end());
    result.maximum = *std::max_element(intervals.begin(), intervals.end());
    for (const auto interval : intervals) {
      const auto delta = interval - mean;
      mean += delta / static_cast<double>(++count);
      m2 += delta * (interval - mean);
    }
    result.rate = mean > 0 ? 1.0 / mean : 0;
    result.deviation = std::sqrt(std::max(0.0, m2 / static_cast<double>(count)));
    return result;
  }

 private:
  void ObserveLocked(Clock::time_point time) {
    ++received_;
    if (last_) {
      if (time < *last_) intervals_.clear();
      else intervals_.push_back(std::chrono::duration<double>(time - *last_).count());
    }
    last_ = time;
    if (intervals_.size() >= window_) intervals_.pop_front();
  }

  const size_t window_;
  mutable std::mutex mutex_;
  uint64_t received_ = 0;
  std::optional<Clock::time_point> last_;
  std::deque<double> intervals_;
};

}  // namespace ibmw::cli::topic
