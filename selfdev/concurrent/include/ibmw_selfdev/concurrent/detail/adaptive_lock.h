// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <mutex>

namespace ibmw::selfdev::concurrent::detail {

/// Spin-wait hint for the current CPU (no-op where none is known).
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#endif
}

/// std::mutex that spins briefly before blocking. The critical sections in
/// this library are tens of nanoseconds; under contention a plain std::mutex
/// turns most of them into a futex sleep/wake pair costing microseconds.
///
/// Waiters spin on a separate `held_` flag with plain loads (no read-modify-
/// write traffic on the lock word while it is held) and only try the mutex
/// once it looks free. The spin is bounded and then falls back to blocking,
/// so a preempted holder (e.g. under SCHED_FIFO on the same CPU) is never
/// starved by spinners. Meets the Lockable requirements.
class AdaptiveMutex {
 public:
  /// ~1-2 us of spinning on current x86 cores before blocking.
  static constexpr int kSpins = 128;

  AdaptiveMutex() = default;
  AdaptiveMutex(const AdaptiveMutex&) = delete;
  AdaptiveMutex& operator=(const AdaptiveMutex&) = delete;

  void lock() {
    for (int i = 0; i < kSpins; ++i) {
      if (!held_.load(std::memory_order_relaxed) && mu_.try_lock()) {
        held_.store(true, std::memory_order_relaxed);
        return;
      }
      CpuRelax();
    }
    mu_.lock();
    held_.store(true, std::memory_order_relaxed);
  }

  bool try_lock() {
    if (held_.load(std::memory_order_relaxed) || !mu_.try_lock()) return false;
    held_.store(true, std::memory_order_relaxed);
    return true;
  }

  void unlock() {
    held_.store(false, std::memory_order_relaxed);
    mu_.unlock();
  }

 private:
  // A hint only; mu_ provides the mutual exclusion and memory ordering.
  std::atomic<bool> held_{false};
  std::mutex mu_;
};

}  // namespace ibmw::selfdev::concurrent::detail
