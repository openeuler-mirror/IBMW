// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
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
  RpcTimeoutTimer() : state_(std::make_shared<State>()) {
    thread_ = std::thread([state = state_] { Run(*state); });
    thread_id_.store(thread_.get_id());
  }
  ~RpcTimeoutTimer() {
    Stop();
    // Destroyed by its own task: the thread ends after it, holding the state.
    std::lock_guard lock(join_mutex_);
    if (thread_.joinable()) thread_.detach();
  }

  RpcTimeoutTimer(const RpcTimeoutTimer&) = delete;
  RpcTimeoutTimer& operator=(const RpcTimeoutTimer&) = delete;

  ibmw::executor::ExecutorHandle Handle() const { return ibmw::executor::ExecutorHandle(&base_); }

  // Waits for a running task; pending tasks are dropped. Called from a task
  // (a timeout callback that shuts the driver down) it only stops the timer:
  // the thread ends after the task, and a Stop from another thread waits
  // for that.
  void Stop() noexcept {
    {
      std::lock_guard lock(state_->mutex);
      state_->stop = true;
    }
    state_->cv.notify_all();
    if (std::this_thread::get_id() != thread_id_.load()) {
      std::lock_guard lock(join_mutex_);
      if (thread_.joinable()) {
        thread_.join();
        thread_id_.store(std::thread::id());  // ids are reused once the thread ended
      }
    }
    std::vector<Entry> dropped;
    {
      std::lock_guard lock(state_->mutex);
      dropped.swap(state_->heap);
    }
  }

 private:
  using Clock = std::chrono::steady_clock;

  // Longest delay kept; a larger one (an "infinite" timeout) is capped.
  static constexpr uint64_t kMaxDelayNs = 100ULL * 365 * 24 * 3600 * 1000000000ULL;

  struct Entry {
    Clock::time_point deadline;
    ibmw::executor::Task task;
  };

  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Entry> heap;
    bool stop = false;
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
  // Throws only if the heap cannot grow (the task is destroyed then).
  void Add(uint64_t system_ns, ibmw_function_base_t* native_task) {
    ibmw::executor::Task task(native_task);
    const uint64_t now_ns = SystemNowNs();
    const uint64_t delay_ns = system_ns > now_ns ? std::min(system_ns - now_ns, kMaxDelayNs) : 0;
    const auto deadline = Clock::now() + std::chrono::nanoseconds(delay_ns);
    State& state = *state_;
    bool earliest = false;
    {
      std::lock_guard lock(state.mutex);
      if (state.stop) return;  // task destroyed after the lock is released
      state.heap.push_back(Entry{deadline, std::move(task)});
      std::push_heap(state.heap.begin(), state.heap.end(), Later);
      earliest = state.heap.front().deadline == deadline;
    }
    // Only a new earliest deadline shortens the wait.
    if (earliest) state.cv.notify_one();
  }

  static void Run(State& state) {
    std::unique_lock lock(state.mutex);
    while (!state.stop) {
      if (state.heap.empty()) {
        state.cv.wait(lock);
        continue;
      }
      // A copy: the heap may grow (and move) while the lock is released.
      const auto deadline = state.heap.front().deadline;
      if (Clock::now() < deadline) {
        state.cv.wait_until(lock, deadline);
        continue;
      }
      std::pop_heap(state.heap.begin(), state.heap.end(), Later);
      {
        ibmw::executor::Task task = std::move(state.heap.back().task);
        state.heap.pop_back();
        lock.unlock();
        task();
      }  // destroyed unlocked too
      lock.lock();
    }
  }

  std::shared_ptr<State> state_;
  std::mutex join_mutex_;  // thread_ is joined by whichever other thread stops first
  std::thread thread_;
  std::atomic<std::thread::id> thread_id_;  // set before any task can run
  const ibmw_executor_base_t base_{
      .struct_size = sizeof(ibmw_executor_base_t),
      .type = [](void*) { return ibmw::util::ToIbmwStringView("dds_rpc_timeout"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("dds_rpc_timeout"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor =
          [](void* ctx) { return std::this_thread::get_id() == Self(ctx)->thread_id_.load(); },
      .supports_timed_dispatch = [](void*) { return true; },
      .dispatch = [](void* ctx, ibmw_function_base_t* task) { Self(ctx)->Add(SystemNowNs(), task); },
      .now = [](void*) { return SystemNowNs(); },
      .dispatch_at_ns = [](void* ctx, uint64_t ns, ibmw_function_base_t* task) { Self(ctx)->Add(ns, task); },
      .ctx = this};
};

}  // namespace ibmw::extensions::dds_extension
