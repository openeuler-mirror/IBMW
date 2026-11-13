// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Test utility: a small thread pool exposing the selfdev Scheduler /
// TimedScheduler interface, so coro tests do not depend on IBMW executors.

#pragma once

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "ibmw_selfdev/coro/export.h"
#include "ibmw_selfdev/coro/task.h"

namespace ibmw::selfdev::coro::test {

class ThreadPool {
 public:
  using Clock = std::chrono::system_clock;

  explicit ThreadPool(size_t threads) {
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { Run(); });
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Notifies under the lock: Post may run on a thread outside the pool (e.g.
  // a detached thread that resumed a coroutine), and once the posted work
  // runs, the pool may be destroyed; ~ThreadPool takes mu_ first, so it never
  // destroys cv_ while a notify is in flight.
  void Post(std::coroutine_handle<> h, Clock::time_point when = Clock::time_point::min()) {
    std::lock_guard<std::mutex> lock(mu_);
    items_.push(Item{when, seq_++, h});
    cv_.notify_one();
  }

  // Uses CurrentThreadId(): called from coroutine bodies, see task.h.
  bool IsWorkerThread() const {
    for (const auto& t : workers_) {
      if (t.get_id() == ::ibmw::selfdev::coro::detail::CurrentThreadId()) return true;
    }
    return false;
  }

  class Scheduler {
   public:
    explicit Scheduler(ThreadPool* pool) noexcept : pool_(pool) {}

    // Follows the awaiter rules of README.md: not an aggregate; await_suspend
    // is out of line and uses only local copies, because a worker may resume
    // h (and destroy the frame holding *this) while Post() is still running.
    class Awaiter {
     public:
      Awaiter(ThreadPool* pool, Clock::time_point when) noexcept : pool_(pool), when_(when) {}
      bool await_ready() const noexcept { return false; }
      IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) const {
        ThreadPool* const pool = pool_;
        const Clock::time_point when = when_;
        pool->Post(h, when);
      }
      void await_resume() const noexcept {}

     private:
      ThreadPool* pool_;
      Clock::time_point when_;
    };

    Awaiter schedule() const noexcept { return Awaiter(pool_, Clock::time_point::min()); }
    Awaiter schedule_after(std::chrono::nanoseconds d) const {
      return Awaiter(pool_, Clock::now() + std::chrono::duration_cast<Clock::duration>(d));
    }
    Awaiter schedule_at(Clock::time_point tp) const noexcept { return Awaiter(pool_, tp); }

    friend bool operator==(const Scheduler& a, const Scheduler& b) noexcept { return a.pool_ == b.pool_; }

   private:
    ThreadPool* pool_;
  };

  Scheduler GetScheduler() noexcept { return Scheduler(this); }

 private:
  struct Item {
    Clock::time_point when;
    size_t seq;
    std::coroutine_handle<> h;
    bool operator>(const Item& o) const { return when != o.when ? when > o.when : seq > o.seq; }
  };

  void Run() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
      if (items_.empty()) {
        if (stop_) return;
        cv_.wait(lock);
        continue;
      }
      auto when = items_.top().when;
      if (when != Clock::time_point::min() && when > Clock::now()) {
        cv_.wait_until(lock, when);
        continue;
      }
      auto h = items_.top().h;
      items_.pop();
      lock.unlock();
      h.resume();
      lock.lock();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> items_;
  size_t seq_ = 0;
  bool stop_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace ibmw::selfdev::coro::test
