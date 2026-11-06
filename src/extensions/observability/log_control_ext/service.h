// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <stdexcept>
#include <unordered_map>

#include "engine/logger/logging_subsystem.h"
#include "cpp/service/rpc_handle.h"
#include "log_control_ext/log_control_type_support.h"
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
      : ibmw::rpc::ServiceBase("json", ibmw::protocols::log_control_extension::kServiceName) {
    using namespace ibmw::protocols::log_control_extension;
    RegisterMethod<GetModuleLogLevelReq, GetModuleLogLevelRsp>(
        "GetModuleLogLevel", &LogControlServiceImpl::HandleGetModuleLogLevel);
    RegisterMethod<SetModuleLogLevelReq, SetModuleLogLevelRsp>(
        "SetModuleLogLevel", &LogControlServiceImpl::HandleSetModuleLogLevel);
  }

  ~LogControlServiceImpl() override = default;

  void SetLoggingSubsystem(ibmw::runtime::core::logger::LoggingSubsystem* ptr) {
    logging_subsystem_ptr_ = ptr;
  }

 private:
  template <typename Req, typename Rsp>
  void RegisterMethod(std::string_view name, void (LogControlServiceImpl::*handler)(const Req&, Rsp&)) {
    RegisterServiceFunc(name, nullptr, GetLogControlTypeSupport<Req>(), GetLogControlTypeSupport<Rsp>(),
        ibmw::rpc::ServiceFunc([this, handler](const ibmw_service_context_base_t*, const void* req,
                                             void* rsp, ibmw_function_base_t* callback) {
          uint32_t status = 0;
          try {
            if (!logging_subsystem_ptr_) throw std::logic_error("Logging subsystem not initialized.");
            (this->*handler)(*static_cast<const Req*>(req), *static_cast<Rsp*>(rsp));
          } catch (...) {
            status = IBMW_RPC_STATUS_SVR_HANDLE_FAILED;
          }
          const auto* ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
          ops->invoker(callback->object_buf, status);
        }));
  }

  void HandleGetModuleLogLevel(
      const ibmw::protocols::log_control_extension::GetModuleLogLevelReq& req,
      ibmw::protocols::log_control_extension::GetModuleLogLevelRsp& rsp);

  void HandleSetModuleLogLevel(
      const ibmw::protocols::log_control_extension::SetModuleLogLevelReq& req,
      ibmw::protocols::log_control_extension::SetModuleLogLevelRsp& rsp);

  enum class ErrorCode : uint32_t {
    kSuc = 0,
    kInvalidModuleName = 1,
    kInvalidLogLevel = 2,
    kEmptySettings = 3,
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_MODULE_NAME",
      "INVALID_LOG_LEVEL",
      "EMPTY_SETTINGS"};

  template <typename Rsp>
  void SetErrorCode(ErrorCode code, Rsp& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  ibmw::runtime::core::logger::LoggingSubsystem* logging_subsystem_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::log_control_extension
