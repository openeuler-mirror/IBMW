// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <exception>
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
          const uint32_t status = Invoke(handler, req, rsp);
          const auto* ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
          ops->invoker(callback->object_buf, status);
        }));
  }

 protected:
  // Runs a handler so that the callback is always invoked. The handlers answer
  // NOT_READY themselves without a logging subsystem (EXT-014); an exception is
  // logged and answered with IBMW_RPC_STATUS_SVR_HANDLE_FAILED and
  // INTERNAL_ERROR, discarding a partly filled response (as parameter_ext).
  template <typename Req, typename Rsp>
  uint32_t Invoke(void (LogControlServiceImpl::*handler)(const Req&, Rsp&), const void* req,
                  void* rsp) noexcept {
    auto& typed_rsp = *static_cast<Rsp*>(rsp);
    try {
      (this->*handler)(*static_cast<const Req*>(req), typed_rsp);
      return 0;
    } catch (const std::exception& e) {
      LogHandlerFailure(e.what());
    } catch (...) {
      LogHandlerFailure("unknown exception");
    }
    try {
      typed_rsp = Rsp{};
      SetErrorCode(ErrorCode::kInternalError, typed_rsp);
    } catch (...) {
      // Out of memory: the status alone reports the failure.
    }
    return IBMW_RPC_STATUS_SVR_HANDLE_FAILED;
  }

  static void LogHandlerFailure(const char* what) noexcept;

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
    kNotReady = 4,
    kInternalError = 5,
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_MODULE_NAME",
      "INVALID_LOG_LEVEL",
      "EMPTY_SETTINGS",
      "NOT_READY",
      "INTERNAL_ERROR"};

  template <typename Rsp>
  void SetErrorCode(ErrorCode code, Rsp& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

 private:
  ibmw::runtime::core::logger::LoggingSubsystem* logging_subsystem_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::log_control_extension
