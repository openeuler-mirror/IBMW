// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdio>
#include <unordered_set>

#include "c/service/rpc_handle_base.h"
#include "engine/service/service_driver_dispatcher.h"

namespace ibmw::runtime::core::service {

class ServiceAccess {
 public:
  ServiceAccess(
      std::string_view pkg_path,
      std::string_view module_name,
      ServiceDriverDispatcher& rpc_driver_bus,
      const std::unordered_set<std::string>& passed_context_meta_keys)
      : pkg_path_(pkg_path),
        module_name_(module_name),
        service_driver_dispatcher_(rpc_driver_bus),
        passed_context_meta_keys_(passed_context_meta_keys),
        base_(GenBase(this)) {}

  ~ServiceAccess() = default;

  ServiceAccess(const ServiceAccess&) = delete;
  ServiceAccess& operator=(const ServiceAccess&) = delete;

  const ibmw_service_handle_base_t* NativeHandle() const { return &base_; }

 private:
  bool RegisterServiceFunc(
      ibmw_string_view_t func_name,
      const void* custom_type_support_ptr,
      const ibmw_type_support_base_t* req_type_support,
      const ibmw_type_support_base_t* rsp_type_support,
      ibmw_function_base_t* service_func) noexcept {
    return service_driver_dispatcher_.RegisterServiceFunc(
        RegisterServiceFuncAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .func_name = func_name,
            .custom_type_support_ptr = custom_type_support_ptr,
            .req_type_support = req_type_support,
            .rsp_type_support = rsp_type_support,
            .service_func = service_func});
  }

  bool RegisterClientFunc(
      ibmw_string_view_t func_name,
      const void* custom_type_support_ptr,
      const ibmw_type_support_base_t* req_type_support,
      const ibmw_type_support_base_t* rsp_type_support) noexcept {
    return service_driver_dispatcher_.RegisterClientFunc(
        RegisterClientFuncAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .func_name = func_name,
            .custom_type_support_ptr = custom_type_support_ptr,
            .req_type_support = req_type_support,
            .rsp_type_support = rsp_type_support});
  }

  void Call(
      ibmw_string_view_t func_name,
      const ibmw_service_context_base_t* ctx_ptr,
      const void* req_ptr,
      void* rsp_ptr,
      ibmw_function_base_t* callback) noexcept {
    // The dispatcher reports failures through the callback; this only guards
    // against allocation failure before it could take the callback over.
    try {
      service_driver_dispatcher_.Call(
          CallAccessInfoWrapper{
              .pkg_path = pkg_path_,
              .module_name = module_name_,
              .func_name = func_name,
              .ctx_ptr = ctx_ptr,
              .req_ptr = req_ptr,
              .rsp_ptr = rsp_ptr,
              .callback = callback});
    } catch (...) {
      std::fprintf(stderr, "IBMW: service call setup failed before dispatch.\n");
    }
  }

  void MergeServerContextToClientContext(
      const ibmw_service_context_base_t* server_ctx_ptr, const ibmw_service_context_base_t* client_ctx_ptr) {
    ibmw::rpc::ContextRef server_ctx_ref(server_ctx_ptr);
    ibmw::rpc::ContextRef client_ctx_ref(client_ctx_ptr);

    if (server_ctx_ref.GetType() != ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT ||
        client_ctx_ref.GetType() != ibmw_rpc_context_type_t::IBMW_RPC_CLIENT_CONTEXT) [[unlikely]] {
      // TODO warn log
      return;
    }

    for (const auto& key : passed_context_meta_keys_) {
      auto val = server_ctx_ref.GetMetaValue(key);
      if (!val.empty()) client_ctx_ref.SetMetaValue(key, val);
    }
  }

  static ibmw_service_handle_base_t GenBase(void* ctx) {
    return ibmw_service_handle_base_t{
        .register_service_func = [](void* ctx,
                                    ibmw_string_view_t func_name,
                                    const void* custom_type_support_ptr,
                                    const ibmw_type_support_base_t* req_type_support,
                                    const ibmw_type_support_base_t* rsp_type_support,
                                    ibmw_function_base_t* service_func) -> bool {
          return static_cast<ServiceAccess*>(ctx)->RegisterServiceFunc(
              func_name, custom_type_support_ptr, req_type_support, rsp_type_support, service_func);
        },
        .register_client_func = [](void* ctx,
                                   ibmw_string_view_t func_name,
                                   const void* custom_type_support_ptr,
                                   const ibmw_type_support_base_t* req_type_support,
                                   const ibmw_type_support_base_t* rsp_type_support) -> bool {
          return static_cast<ServiceAccess*>(ctx)->RegisterClientFunc(
              func_name, custom_type_support_ptr, req_type_support, rsp_type_support);
        },
        .call = [](void* ctx,
                     ibmw_string_view_t func_name,
                     const ibmw_service_context_base_t* ctx_ptr,
                     const void* req_ptr,
                     void* rsp_ptr,
                     ibmw_function_base_t* callback) {
          static_cast<ServiceAccess*>(ctx)->Call(func_name, ctx_ptr, req_ptr, rsp_ptr, callback);  //
        },
        .merge_server_context_to_client_context = [](void* ctx,                                        //
                                                     const ibmw_service_context_base_t* server_ctx_ptr,     //
                                                     const ibmw_service_context_base_t* client_ctx_ptr) {   //
          static_cast<ServiceAccess*>(ctx)->MergeServerContextToClientContext(server_ctx_ptr, client_ctx_ptr);
        },
        .ctx = ctx};
  }

 private:
  const std::string pkg_path_;
  const std::string module_name_;

  ServiceDriverDispatcher& service_driver_dispatcher_;

  const std::unordered_set<std::string>& passed_context_meta_keys_;

  const ibmw_service_handle_base_t base_;
};

}  // namespace ibmw::runtime::core::service
