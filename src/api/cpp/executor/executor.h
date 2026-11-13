// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>

#include <string_view>

#include "c/executor/executor_base.h"
#include "cpp/util/function.h"
#include "cpp/util/string.h"
#include "base/dynamiclatch.h"
#include "base/exception.h"
#include "base/time_util.h"

namespace ibmw::executor {

using Task = ibmw::util::Function<ibmw_function_executor_task_ops_t>;

class ExecutorHandle {
 public:
  ExecutorHandle() = default;
  explicit ExecutorHandle(const ibmw_executor_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ExecutorHandle() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_executor_base_t* NativeHandle() const { return base_ptr_; }

  std::string_view Type() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return ibmw::util::ToStdStringView(base_ptr_->type(base_ptr_->ctx));
  }

  std::string_view Name() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return ibmw::util::ToStdStringView(base_ptr_->name(base_ptr_->ctx));
  }

  bool ThreadSafe() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->is_thread_safe(base_ptr_->ctx);
  }

  bool IsInCurrentExecutor() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->is_in_current_executor(base_ptr_->ctx);
  }

  bool SupportsTimedDispatch() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->supports_timed_dispatch(base_ptr_->ctx);
  }

  void Dispatch(Task&& task) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->dispatch(base_ptr_->ctx, task.NativeHandle());
  }

  void DispatchOrRun(Task&& task) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    if (base_ptr_->is_in_current_executor(base_ptr_->ctx)) {
      task();
    } else {
      base_ptr_->dispatch(base_ptr_->ctx, task.NativeHandle());
    }
  }

  std::chrono::system_clock::time_point Now() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return common::util::GetTimePointFromTimestampNs(base_ptr_->now(base_ptr_->ctx));
  }

  void DispatchAt(std::chrono::system_clock::time_point tp, Task&& task) {
    MW_ASSERT(base_ptr_, "Reference is null.");

    MW_ASSERT(SupportsTimedDispatch(), "Current executor does not support timer scheduling.");

    base_ptr_->dispatch_at_ns(
        base_ptr_->ctx, common::util::GetTimestampNs(tp), task.NativeHandle());
  }

  void DispatchAfter(std::chrono::nanoseconds dt, Task&& task) {
    DispatchAt(
        Now() + std::chrono::duration_cast<std::chrono::system_clock::time_point::duration>(dt),
        std::move(task));
  }

  bool TryDispatch(util::DynamicLatch& latch, Task&& task) {
    if (!latch.TryAdd()) {
      return false;
    }
    // Task::operator() is noexcept, so the count is only lost if Dispatch itself
    // throws (e.g. null executor); undo the TryAdd so CloseAndWait cannot hang.
    try {
      Dispatch([&latch, task = std::move(task)]() {
        task();
        latch.CountDown();
      });
    } catch (...) {
      latch.CountDown();
      throw;
    }
    return true;
  }

 private:
  const ibmw_executor_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::executor
