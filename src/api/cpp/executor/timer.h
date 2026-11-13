// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "cpp/executor/executor.h"
#include "base/same_arg_trait.h"

namespace ibmw::executor {

class TimerBase {
 public:
  TimerBase(ExecutorHandle executor, std::chrono::nanoseconds period)
      : executor_(executor), period_(period) {}

  virtual ~TimerBase() = default;

  TimerBase(const TimerBase&) = delete;
  TimerBase& operator=(const TimerBase&) = delete;

  virtual void Reset() = 0;

  virtual void ExecuteTask() = 0;

  void Cancel() { cancelled_.store(true, std::memory_order_release); }

  [[nodiscard]] bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }

  [[nodiscard]] std::chrono::nanoseconds Period() const { return period_; }

  [[nodiscard]] std::chrono::system_clock::time_point NextCallTime() const {
    return next_call_time_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::chrono::nanoseconds TimeUntilNextCall() const {
    return NextCallTime() - executor_.Now();
  }

  [[nodiscard]] ExecutorHandle Executor() const { return executor_; }

 protected:
  ExecutorHandle executor_;
  std::chrono::nanoseconds period_;
  // Written by Reset()/Cancel() on user threads and by the timer loop on the
  // executor thread.
  std::atomic<std::chrono::system_clock::time_point> next_call_time_{};
  std::atomic<bool> cancelled_{true};
  // Bumped by every Reset(); only the chain started by the latest Reset() runs.
  // Planned times cannot tell chains apart: a Reset() on the old chain's next
  // tick (e.g. a paused simulated clock) plans the same time point.
  std::atomic<uint64_t> generation_{0};
};

template <typename TaskType>
  requires(common::util::SameArguments<TaskType, std::function<void()>> ||
           common::util::SameArguments<TaskType, std::function<void(TimerBase&)>> ||
           common::util::SameArguments<TaskType, std::function<void(const TimerBase&)>>)
class Timer : public TimerBase, public std::enable_shared_from_this<Timer<TaskType>> {
 public:
  Timer(ExecutorHandle executor, std::chrono::nanoseconds period, TaskType&& task)
      : TimerBase(executor, period), task_(std::forward<TaskType>(task)) {}

  ~Timer() override { Cancel(); }

  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  void Reset() override {
    const auto next = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        executor_.Now() + period_);
    const uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    next_call_time_.store(next, std::memory_order_release);
    cancelled_.store(false, std::memory_order_release);
    ExecuteLoop(next, generation);
  }

  void ExecuteTask() override {
    if constexpr (std::is_invocable_v<TaskType>) {
      task_();
    } else {
      task_(*this);
    }
  }

  [[nodiscard]] const TaskType& Task() const { return task_; }

 private:
  // The loop only holds a weak reference: once the last user handle is gone the
  // Timer is destroyed (~Timer cancels) instead of rescheduling itself forever.
  void ExecuteLoop(std::chrono::system_clock::time_point planned_time, uint64_t generation) {
    auto weak_this = this->weak_from_this();
    MW_ASSERT(!weak_this.expired(), "Timer must be owned by a std::shared_ptr (use CreateTimer).");
    executor_.DispatchAt(planned_time, [weak_this = std::move(weak_this), planned_time, generation]() {
      auto shared_this = weak_this.lock();
      if (!shared_this || shared_this->IsCancelled()) {
        return;
      }

      // Skip current execution if timer was reset or restarted
      if (generation != shared_this->generation_.load(std::memory_order_acquire)) {
        return;
      }

      shared_this->ExecuteTask();

      auto now = shared_this->executor_.Now();
      auto next = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          planned_time + shared_this->period_);

      // If now is ahead of the next call time, skip some times.
      if (next < now) {
        if (shared_this->period_ == std::chrono::nanoseconds::zero()) {
          next = now;
        } else {
          auto now_ahead = now - next;
          auto skip_count = 1 + ((now_ahead - std::chrono::nanoseconds(1)) / shared_this->period_);
          next = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
              next + (skip_count * shared_this->period_));
        }
      }

      // A Reset() during the task owns the schedule from here on; do not start a second chain.
      if (generation != shared_this->generation_.load(std::memory_order_acquire)) {
        return;
      }
      // Fails only when a concurrent Reset() already stored its own time; keep that value.
      auto expected = planned_time;
      shared_this->next_call_time_.compare_exchange_strong(expected, next, std::memory_order_acq_rel);

      shared_this->ExecuteLoop(next, generation);
    });
  }

 private:
  TaskType task_;
};

// The returned handle owns the timer: once the last copy is released the timer
// stops. Keep it (e.g. as a member) for as long as the timer should run.
template <typename TaskType>
[[nodiscard]] std::shared_ptr<TimerBase> CreateTimer(ExecutorHandle executor, std::chrono::nanoseconds period,
                                       TaskType&& task, bool auto_start = true) {
  MW_ASSERT(executor, "Executor is null.");
  MW_ASSERT(executor.SupportsTimedDispatch(), "Executor does not support timer scheduling.");
  MW_ASSERT(period >= std::chrono::nanoseconds::zero(), "Timer period must not be negative.");
  auto timer = std::make_shared<Timer<TaskType>>(executor, period, std::forward<TaskType>(task));
  if (auto_start) {
    timer->Reset();
  }
  return timer;
}

}  // namespace ibmw::executor
