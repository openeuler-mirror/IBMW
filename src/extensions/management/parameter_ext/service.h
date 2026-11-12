// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "engine/module/parameter_store.h"
#include "cpp/service/rpc_handle.h"
#include "parameter_ext/parameter_type_support.h"
#include "parameter_ext/parameter_types.h"

namespace ibmw::extensions::parameter_extension {

/**
 * @brief JSON-based RPC service implementation for parameter management.
 *
 * Replaces the protobuf-generated ParameterServiceCoService.
 * Inherits from ibmw::rpc::ServiceBase directly and registers
 * JSON-compatible RPC handlers in the constructor:
 *   - Set
 *   - Get
 *   - List
 *   - Dump
 *   - Load
 *   - ListModules
 */
class ParameterServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  ParameterServiceImpl()
      : ibmw::rpc::ServiceBase("json", ibmw::protocols::parameter_extension::kServiceName) {
    using namespace ibmw::protocols::parameter_extension;
    RegisterMethod<SetParameterReq, SetParameterRsp>("Set", &ParameterServiceImpl::HandleSet);
    RegisterMethod<GetParameterReq, GetParameterRsp>("Get", &ParameterServiceImpl::HandleGet);
    RegisterMethod<ListParameterReq, ListParameterRsp>("List", &ParameterServiceImpl::HandleList);
    RegisterMethod<DumpParameterReq, DumpParameterRsp>("Dump", &ParameterServiceImpl::HandleDump);
    RegisterMethod<LoadParameterReq, LoadParameterRsp>("Load", &ParameterServiceImpl::HandleLoad);
    RegisterMethod<ListModulesReq, ListModulesRsp>("ListModules", &ParameterServiceImpl::HandleListModules);
  }

  ~ParameterServiceImpl() override = default;

  void SetParameterStore(ibmw::runtime::core::parameter::ParameterStore* ptr) {
    parameter_store_ptr_ = ptr;
  }

 private:
  template <typename Req, typename Rsp>
  void RegisterMethod(std::string_view name, void (ParameterServiceImpl::*handler)(const Req&, Rsp&)) {
    RegisterServiceFunc(name, nullptr, GetParameterTypeSupport<Req>(), GetParameterTypeSupport<Rsp>(),
        ibmw::rpc::ServiceFunc([this, handler](const ibmw_service_context_base_t*, const void* req,
                                             void* rsp, ibmw_function_base_t* callback) {
          const uint32_t status = Invoke(handler, req, rsp);
          const auto* ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
          ops->invoker(callback->object_buf, status);
        }));
  }

 protected:
  // Runs a handler so that the callback is always invoked: an exception
  // (e.g. the parameter store is not started) is logged and answered with
  // IBMW_RPC_STATUS_SVR_HANDLE_FAILED and an error code instead of leaving
  // the RPC without an answer. A partly filled response is discarded; the
  // error text fits the small-string buffer (no allocation). Returns the RPC
  // status.
  template <typename Req, typename Rsp>
  uint32_t Invoke(void (ParameterServiceImpl::*handler)(const Req&, Rsp&), const void* req, void* rsp) noexcept {
    auto& typed_rsp = *static_cast<Rsp*>(rsp);
    if (parameter_store_ptr_ == nullptr) [[unlikely]] {
      SetErrorCode(ErrorCode::kNotReady, typed_rsp);
      return 0;
    }
    try {
      (this->*handler)(*static_cast<const Req*>(req), typed_rsp);
      return 0;
    } catch (const std::exception& e) {
      LogHandlerFailure(e.what());
    } catch (...) {
      LogHandlerFailure("unknown exception");
    }
    typed_rsp = Rsp{};
    SetErrorCode(ErrorCode::kInternalError, typed_rsp);
    return IBMW_RPC_STATUS_SVR_HANDLE_FAILED;
  }

  static void LogHandlerFailure(const char* what) noexcept;

  void HandleListModules(
      const ibmw::protocols::parameter_extension::ListModulesReq& req,
      ibmw::protocols::parameter_extension::ListModulesRsp& rsp);

  void HandleSet(
      const ibmw::protocols::parameter_extension::SetParameterReq& req,
      ibmw::protocols::parameter_extension::SetParameterRsp& rsp);

  void HandleGet(
      const ibmw::protocols::parameter_extension::GetParameterReq& req,
      ibmw::protocols::parameter_extension::GetParameterRsp& rsp);

  void HandleList(
      const ibmw::protocols::parameter_extension::ListParameterReq& req,
      ibmw::protocols::parameter_extension::ListParameterRsp& rsp);

  void HandleDump(
      const ibmw::protocols::parameter_extension::DumpParameterReq& req,
      ibmw::protocols::parameter_extension::DumpParameterRsp& rsp);

  void HandleLoad(
      const ibmw::protocols::parameter_extension::LoadParameterReq& req,
      ibmw::protocols::parameter_extension::LoadParameterRsp& rsp);

  enum class ErrorCode : uint32_t {
    kSuc = 0,
    kInvalidModuleName = 1,
    kParameterNotFound = 2,
    kInvalidParameterName = 3,
    kInternalError = 4,
    kNotReady = 5,
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_MODULE_NAME",
      "PARAMETER_NOT_FOUND",
      "INVALID_PARAMETER_NAME",
      "INTERNAL_ERROR",
      "NOT_READY"};

  template <typename T>
  void SetErrorCode(ErrorCode code, T& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  ibmw::runtime::core::parameter::ParameterStore* parameter_store_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::parameter_extension
