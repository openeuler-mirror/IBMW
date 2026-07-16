// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <unordered_map>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_type_support.h"
#include "time_manipulator_ext/time_manipulator_executor.h"
#include "time_manipulator_ext/time_manipulator_types.h"

namespace ibmw::extensions::time_manipulator_extension {

/**
 * @brief JSON-based RPC service implementation for time manipulation.
 *
 * Replaces the protobuf-generated TimeManipulatorServiceCoService.
 * Inherits from ibmw::rpc::ServiceBase directly and registers
 * three JSON-typed RPC handlers in the constructor:
 *   - SetTimeRatio
 *   - Pause
 *   - GetTimeRatio
 */
class TimeManipulatorServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  TimeManipulatorServiceImpl()
      : ibmw::rpc::ServiceBase(
            "json",
            "ibmw.protocols.time_manipulator_extension.TimeManipulatorService") {
    using namespace ibmw::protocols::time_manipulator_extension;

    // Register SetTimeRatio handler
    RegisterServiceFunc(
        "SetTimeRatio",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<SetTimeRatioReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleSetTimeRatio(
                  *static_cast<const SetTimeRatioReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register Pause handler
    RegisterServiceFunc(
        "Pause",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<PauseReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandlePause(
                  *static_cast<const PauseReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register GetTimeRatio handler
    RegisterServiceFunc(
        "GetTimeRatio",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<GetTimeRatioReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleGetTimeRatio(
                  *static_cast<const GetTimeRatioReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));
  }

  ~TimeManipulatorServiceImpl() override = default;

  void RegisterTimeManipulatorExecutor(TimeManipulatorExecutor* ptr) {
    executor_map_.emplace(ptr->Name(), ptr);
  }

 private:
  void HandleSetTimeRatio(
      const ibmw::protocols::time_manipulator_extension::SetTimeRatioReq& req,
      ibmw::protocols::time_manipulator_extension::CommonRsp& rsp);

  void HandlePause(
      const ibmw::protocols::time_manipulator_extension::PauseReq& req,
      ibmw::protocols::time_manipulator_extension::CommonRsp& rsp);

  void HandleGetTimeRatio(
      const ibmw::protocols::time_manipulator_extension::GetTimeRatioReq& req,
      ibmw::protocols::time_manipulator_extension::CommonRsp& rsp);

  enum class ErrorCode : uint32_t {
    kSuc = 0,
    kInvalidExecutorName = 1,
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_EXECUTOR_NAME"};

  static void SetErrorCode(
      ErrorCode code,
      ibmw::protocols::time_manipulator_extension::CommonRsp& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  std::unordered_map<std::string_view, TimeManipulatorExecutor*> executor_map_;
};

}  // namespace ibmw::extensions::time_manipulator_extension
