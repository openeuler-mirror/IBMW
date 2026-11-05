// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "cpp/executor/executor.h"
#include "cpp/util/string.h"

namespace ibmw::extensions::dds_extension {

// Timed-dispatch executor for the RPC client timeouts when no
// timeout_executor is configured (TRANS-049): a native .srv server cannot
// answer a failure, so without a timeout the call would never complete.
// One thread sleeps until the earliest deadline; nothing runs while idle.
class RpcTimeoutTimer {
 public:
  RpcTimeoutTimer() : thread_([this] { Run(); }) {}
  ~RpcTimeoutTimer() { Stop(); }

  RpcTimeoutTimer(const RpcTimeoutTimer&) = delete;
  RpcTimeoutTimer& operator=(const RpcTimeoutTimer&) = delete;

  ibmw::executor::ExecutorHandle Handle() const { return ibmw::executor::ExecutorHandle(&base_); }

  // Waits for a running task; pending tasks are dropped.
  void Stop() noexcept {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::vector<Entry> dropped;
    {
      std::lock_guard lock(mutex_);
      dropped.swap(heap_);
    }
  }

 private:
  using Clock = std::chrono::steady_clock;

  struct Entry {
    Clock::time_point deadline;
    ibmw::executor::Task task;
  };

  // Min-heap on the deadline.
  static bool Later(const Entry& a, const Entry& b) { return a.deadline > b.deadline; }

  static RpcTimeoutTimer* Self(void* ctx) { return static_cast<RpcTimeoutTimer*>(ctx); }

  static uint64_t SystemNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // The deadline is given on the system clock; it is kept on the steady one.
  void Add(uint64_t system_ns, ibmw_function_base_t* native_task) {
    ibmw::executor::Task task(native_task);
    const uint64_t now_ns = SystemNowNs();
    const auto delay = std::chrono::nanoseconds(system_ns > now_ns ? system_ns - now_ns : 0);
    const auto deadline = Clock::now() + delay;
    bool earliest = false;
    {
      std::lock_guard lock(mutex_);
      if (stop_) return;  // task destroyed after the lock is released
      heap_.push_back(Entry{deadline, std::move(task)});
      std::push_heap(heap_.begin(), heap_.end(), Later);
      earliest = heap_.front().deadline == deadline;
    }
    // Only a new earliest deadline shortens the wait.
    if (earliest) cv_.notify_one();
  }

  void Run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
      if (heap_.empty()) {
        cv_.wait(lock);
        continue;
      }
      if (Clock::now() < heap_.front().deadline) {
        cv_.wait_until(lock, heap_.front().deadline);
        continue;
      }
      std::pop_heap(heap_.begin(), heap_.end(), Later);
      {
        ibmw::executor::Task task = std::move(heap_.back().task);
        heap_.pop_back();
        lock.unlock();
        task();
      }  // destroyed unlocked too
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Entry> heap_;
  bool stop_ = false;
  const ibmw_executor_base_t base_{
      .struct_size = sizeof(ibmw_executor_base_t),
      .type = [](void*) { return ibmw::util::ToIbmwStringView("dds_rpc_timeout"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("dds_rpc_timeout"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor = [](void* ctx) { return std::this_thread::get_id() == Self(ctx)->thread_.get_id(); },
      .supports_timed_dispatch = [](void*) { return true; },
      .dispatch = [](void* ctx, ibmw_function_base_t* task) { Self(ctx)->Add(SystemNowNs(), task); },
      .now = [](void*) { return SystemNowNs(); },
      .dispatch_at_ns = [](void* ctx, uint64_t ns, ibmw_function_base_t* task) { Self(ctx)->Add(ns, task); },
      .ctx = this};
  std::thread thread_;  // last: starts after the members above
};

}  // namespace ibmw::extensions::dds_extension
