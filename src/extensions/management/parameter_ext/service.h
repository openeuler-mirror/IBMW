// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "engine/module/parameter_store.h"
#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_type_support.h"
#include "parameter_ext/parameter_types.h"

namespace ibmw::extensions::parameter_extension {

/**
 * @brief JSON-based RPC service implementation for parameter management.
 *
 * Replaces the protobuf-generated ParameterServiceCoService.
 * Inherits from ibmw::rpc::ServiceBase directly and registers
 * five JSON-typed RPC handlers in the constructor:
 *   - Set
 *   - Get
 *   - List
 *   - Dump
 *   - Load
 */
class ParameterServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  ParameterServiceImpl()
      : ibmw::rpc::ServiceBase(
            "json",
            "ibmw.protocols.parameter_extension.ParameterService") {
    using namespace ibmw::protocols::parameter_extension;

    // Register Set handler
    RegisterServiceFunc(
        "Set",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<SetParameterReq>(),
        ibmw::json::GetJsonMessageTypeSupport<SetParameterRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleSet(
                  *static_cast<const SetParameterReq*>(req),
                  *static_cast<SetParameterRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register Get handler
    RegisterServiceFunc(
        "Get",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<GetParameterReq>(),
        ibmw::json::GetJsonMessageTypeSupport<GetParameterRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleGet(
                  *static_cast<const GetParameterReq*>(req),
                  *static_cast<GetParameterRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register List handler
    RegisterServiceFunc(
        "List",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<ListParameterReq>(),
        ibmw::json::GetJsonMessageTypeSupport<ListParameterRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleList(
                  *static_cast<const ListParameterReq*>(req),
                  *static_cast<ListParameterRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register Dump handler
    RegisterServiceFunc(
        "Dump",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<DumpParameterReq>(),
        ibmw::json::GetJsonMessageTypeSupport<DumpParameterRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleDump(
                  *static_cast<const DumpParameterReq*>(req),
                  *static_cast<DumpParameterRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register Load handler
    RegisterServiceFunc(
        "Load",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<LoadParameterReq>(),
        ibmw::json::GetJsonMessageTypeSupport<LoadParameterRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req,
                   void* rsp,
                   ibmw_function_base_t* callback) {
              HandleLoad(
                  *static_cast<const LoadParameterReq*>(req),
                  *static_cast<LoadParameterRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));
  }

  ~ParameterServiceImpl() override = default;

  void SetParameterStore(ibmw::runtime::core::parameter::ParameterStore* ptr) {
    parameter_store_ptr_ = ptr;
  }

 private:
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
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_MODULE_NAME"};

  template <typename T>
  void SetErrorCode(ErrorCode code, T& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  ibmw::runtime::core::parameter::ParameterStore* parameter_store_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::parameter_extension
