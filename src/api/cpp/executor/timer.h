// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
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

  void Cancel() { cancelled_ = true; }

  [[nodiscard]] bool IsCancelled() const { return cancelled_; }

  [[nodiscard]] std::chrono::nanoseconds Period() const { return period_; }

  [[nodiscard]] std::chrono::system_clock::time_point NextCallTime() const {
    return next_call_time_;
  }

  [[nodiscard]] std::chrono::nanoseconds TimeUntilNextCall() const {
    return next_call_time_ - executor_.Now();
  }

  [[nodiscard]] ExecutorHandle Executor() const { return executor_; }

 protected:
  ExecutorHandle executor_;
  std::chrono::nanoseconds period_;
  std::chrono::system_clock::time_point next_call_time_;
  bool cancelled_ = true;
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
    cancelled_ = false;
    next_call_time_ = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        executor_.Now() + period_);
    ExecuteLoop();
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
  void ExecuteLoop() {
    auto shared_this = this->shared_from_this();
    executor_.DispatchAt(next_call_time_, [shared_this, planned_time = next_call_time_]() {
      if (shared_this->IsCancelled()) {
        return;
      }

      // Skip current execution if timer was reset or restarted
      if (planned_time != shared_this->next_call_time_) {
        return;
      }

      shared_this->ExecuteTask();

      auto now = shared_this->executor_.Now();
      shared_this->next_call_time_ = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          shared_this->next_call_time_ + shared_this->period_);

      // If now is ahead of the next call time, skip some times.
      if (shared_this->next_call_time_ < now) {
        if (shared_this->period_ == std::chrono::nanoseconds::zero()) {
          shared_this->next_call_time_ = now;
        } else {
          auto now_ahead = now - shared_this->next_call_time_;
          auto skip_count = 1 + ((now_ahead - std::chrono::nanoseconds(1)) / shared_this->period_);
          shared_this->next_call_time_ = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
              shared_this->next_call_time_ + (skip_count * shared_this->period_));
        }
      }

      shared_this->ExecuteLoop();
    });
  }

 private:
  TaskType task_;
};

template <typename TaskType>
std::shared_ptr<TimerBase> CreateTimer(ExecutorHandle executor, std::chrono::nanoseconds period,
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
