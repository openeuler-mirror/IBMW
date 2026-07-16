// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>

#include "engine/logger/logging_subsystem.h"
#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_type_support.h"
#include "log_control_ext/log_control_types.h"

namespace ibmw::extensions::log_control_extension {

/**
 * @brief JSON-based RPC service implementation for log control.
 *
 * Replaces the protobuf-generated LogControlServiceCoService.
 * Inherits from ibmw::rpc::ServiceBase directly and registers
 * two JSON-typed RPC handlers in the constructor:
 *   - GetModuleLogLevel
 *   - SetModuleLogLevel
 */
class LogControlServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  LogControlServiceImpl()
      : ibmw::rpc::ServiceBase(
            "json",
            "ibmw.protocols.log_control_extension.LogControlService") {
    // Register GetModuleLogLevel handler
    RegisterServiceFunc(
        "GetModuleLogLevel",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<
            ibmw::protocols::log_control_extension::GetModuleLogLevelReq>(),
        ibmw::json::GetJsonMessageTypeSupport<
            ibmw::protocols::log_control_extension::GetModuleLogLevelRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleGetModuleLogLevel(
                  *static_cast<const ibmw::protocols::log_control_extension::GetModuleLogLevelReq*>(req),
                  *static_cast<ibmw::protocols::log_control_extension::GetModuleLogLevelRsp*>(rsp));
              // Signal success (status code 0 = OK)
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register SetModuleLogLevel handler
    RegisterServiceFunc(
        "SetModuleLogLevel",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<
            ibmw::protocols::log_control_extension::SetModuleLogLevelReq>(),
        ibmw::json::GetJsonMessageTypeSupport<
            ibmw::protocols::log_control_extension::SetModuleLogLevelRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleSetModuleLogLevel(
                  *static_cast<const ibmw::protocols::log_control_extension::SetModuleLogLevelReq*>(req),
                  *static_cast<ibmw::protocols::log_control_extension::SetModuleLogLevelRsp*>(rsp));
              // Signal success (status code 0 = OK)
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));
  }

  ~LogControlServiceImpl() override = default;

  void SetLoggingSubsystem(ibmw::runtime::core::logger::LoggingSubsystem* ptr) {
    logging_subsystem_ptr_ = ptr;
  }

 private:
  void HandleGetModuleLogLevel(
      const ibmw::protocols::log_control_extension::GetModuleLogLevelReq& req,
      ibmw::protocols::log_control_extension::GetModuleLogLevelRsp& rsp);

  void HandleSetModuleLogLevel(
      const ibmw::protocols::log_control_extension::SetModuleLogLevelReq& req,
      ibmw::protocols::log_control_extension::SetModuleLogLevelRsp& rsp);

  enum class ErrorCode : uint32_t {
    kSuc = 0,
    kInvalidModuleName = 1,
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_MODULE_NAME"};

  void SetErrorCode(ErrorCode code,
                    ibmw::protocols::log_control_extension::GetModuleLogLevelRsp& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  void SetErrorCode(ErrorCode code,
                    ibmw::protocols::log_control_extension::SetModuleLogLevelRsp& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  ibmw::runtime::core::logger::LoggingSubsystem* logging_subsystem_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::log_control_extension
