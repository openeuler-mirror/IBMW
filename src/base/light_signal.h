// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace ibmw::common::util {

// A manual-reset event: Notify wakes every current and later waiter until
// Reset. A waiter that starts after Reset waits for the next Notify, so a
// Notify followed by Reset before a slow waiter checks the flag is missed by
// that waiter.
class LightSignal {
 public:
  LightSignal() = default;
  ~LightSignal() = default;

  void Notify() {
    std::lock_guard<std::mutex> lck(mutex_);
    flag_ = true;
    cond_.notify_all();
  }

  void Wait() {
    std::unique_lock<std::mutex> lck(mutex_);
    cond_.wait(lck, [this] { return flag_; });
  }

  bool WaitFor(std::chrono::nanoseconds timeout) {
    std::unique_lock<std::mutex> lck(mutex_);
    return cond_.wait_for(lck, timeout, [this] { return flag_; });
  }

  void Reset() {
    std::lock_guard<std::mutex> lck(mutex_);
    flag_ = false;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  bool flag_ = false;
};

}  // namespace ibmw::common::util
