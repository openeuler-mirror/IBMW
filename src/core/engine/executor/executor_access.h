// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <unordered_map>

#include "c/executor/executor_bus_base.h"
#include "engine/executor/executor_base.h"
#include "base/log_util.h"
#include "base/string_util.h"
#include "base/time_util.h"

namespace ibmw::runtime::core::executor {

class ExecutorAccess {
 public:
  explicit ExecutorAccess(ExecutorBase* executor_ptr)
      : base_(GenBase(executor_ptr)) {}

  ~ExecutorAccess() = default;

  ExecutorAccess(const ExecutorAccess&) = delete;
  ExecutorAccess& operator=(const ExecutorAccess&) = delete;

  const ibmw_executor_base_t* NativeHandle() const { return &base_; }

 private:
  static ibmw_executor_base_t GenBase(void* ctx) {
    return ibmw_executor_base_t{
        .type = [](void* ctx) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(
              static_cast<ExecutorBase*>(ctx)->Type());
        },
        .name = [](void* ctx) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(
              static_cast<ExecutorBase*>(ctx)->Name());
        },
        .is_thread_safe = [](void* ctx) -> bool {
          return static_cast<ExecutorBase*>(ctx)->ThreadSafe();
        },
        .is_in_current_executor = [](void* ctx) -> bool {
          return static_cast<ExecutorBase*>(ctx)->IsInCurrentExecutor();
        },
        .supports_timed_dispatch = [](void* ctx) -> bool {
          return static_cast<ExecutorBase*>(ctx)->SupportsTimedDispatch();
        },
        .dispatch = [](void* ctx, ibmw_function_base_t* task) {
          static_cast<ExecutorBase*>(ctx)->Dispatch(ibmw::executor::Task(task));  //
        },
        .now = [](void* ctx) -> uint64_t {
          return ibmw::common::util::GetTimestampNs(static_cast<ExecutorBase*>(ctx)->Now());
        },
        .dispatch_at_ns = [](void* ctx, uint64_t tp, ibmw_function_base_t* task) {
          static_cast<ExecutorBase*>(ctx)->DispatchAt(
              ibmw::common::util::GetTimePointFromTimestampNs(tp), ibmw::executor::Task(task));  //
        },
        .ctx = ctx};
  }

 private:
  const ibmw_executor_base_t base_;
};

class ExecutorBusAccess {
 public:
  using ExecutorAccessMap = std::unordered_map<
      std::string, std::unique_ptr<ExecutorAccess>, ibmw::common::util::StringHash, std::equal_to<>>;

 public:
  explicit ExecutorBusAccess(const ExecutorAccessMap& executor_access_map)
      : executor_access_map_(executor_access_map),
        base_(GenBase(this)) {}

  ~ExecutorBusAccess() = default;

  ExecutorBusAccess(const ExecutorBusAccess&) = delete;
  ExecutorBusAccess& operator=(const ExecutorBusAccess&) = delete;

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  const ibmw_executor_bus_base_t* NativeHandle() const { return &base_; }

 private:
  const ibmw_executor_base_t* GetExecutor(ibmw_string_view_t executor_name) const {
    auto finditr = executor_access_map_.find(ibmw::util::ToStdStringView(executor_name));
    if (finditr != executor_access_map_.end()) return finditr->second->NativeHandle();

    MW_WARN("Can not find executor '{}'.", ibmw::util::ToStdStringView(executor_name));

    return nullptr;
  }

  static ibmw_executor_bus_base_t GenBase(void* ctx) {
    return ibmw_executor_bus_base_t{
        .get_executor = [](void* ctx, ibmw_string_view_t executor_name) -> const ibmw_executor_base_t* {
          return static_cast<ExecutorBusAccess*>(ctx)->GetExecutor(executor_name);
        },
        .ctx = ctx};
  }

 private:
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;
  const ExecutorAccessMap& executor_access_map_;
  const ibmw_executor_bus_base_t base_;
};

}  // namespace ibmw::runtime::core::executor
