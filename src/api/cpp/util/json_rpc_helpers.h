// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// JSON RPC convenience helpers — type-safe wrappers that eliminate void*
// casts, raw C ABI callback invocations, and manual promise/future ceremony.
//
// Server side:
//   ibmw::json::rpc::RegisterServiceFunc<ReqT, RspT>(
//       service, "FuncName",
//       [](const ReqT& req, RspT& rsp) -> uint32_t { ... return 0; });
//
// Client side (registration):
//   ibmw::json::rpc::RegisterClientFunc<ReqT, RspT>(
//       service_ref, "ServiceName", "FuncName");
//
// Client side (synchronous call):
//   auto status = ibmw::json::rpc::Call<ReqT, RspT>(
//       service_ref, "ServiceName", "FuncName", req, rsp);
//
// Client side (synchronous call with timeout):
//   auto status = ibmw::json::rpc::Call<ReqT, RspT>(
//       service_ref, "ServiceName", "FuncName", req, rsp,
//       std::chrono::milliseconds(3000));

#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <string>

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_context.h"
#include "cpp/util/json_type_support.h"

namespace ibmw::json::rpc {

// ── Server: typed RegisterServiceFunc ───────────────────────────────────────

/**
 * @brief Register a JSON RPC service function with type-safe handler.
 *
 * Eliminates void* casts, raw C ABI callback invocations, and manual
 * type support lookups. The handler receives typed references and returns
 * a status code directly.
 *
 * @tparam ReqType  Request struct (must satisfy JsonMessage concept).
 * @tparam RspType  Response struct (must satisfy JsonMessage concept).
 * @param service   The ServiceBase to register on.
 * @param func_name The function name (e.g. "Compute").
 * @param handler   Business logic: receives (const ReqType&, RspType&),
 *                  returns uint32_t status code (0 = success).
 */
template <typename ReqType, typename RspType>
void RegisterServiceFunc(
    ibmw::rpc::ServiceBase& service,
    std::string_view func_name,
    std::function<uint32_t(const ReqType&, RspType&)>&& handler) {
  service.RegisterServiceFunc(
      func_name,
      nullptr,
      ibmw::json::GetJsonMessageTypeSupport<ReqType>(),
      ibmw::json::GetJsonMessageTypeSupport<RspType>(),
      ibmw::rpc::ServiceFunc(
          [handler{std::move(handler)}](
              const ibmw_service_context_base_t* /*ctx_ptr*/,
              const void* req_ptr,
              void* rsp_ptr,
              ibmw_function_base_t* callback) {
            auto& req = *static_cast<const ReqType*>(req_ptr);
            auto& rsp = *static_cast<RspType*>(rsp_ptr);

            uint32_t code = handler(req, rsp);

            auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(
                callback->ops);
            ops->invoker(callback->object_buf, code);
          }));
}

/**
 * @brief Overload with context-aware handler.
 *
 * The handler additionally receives the RPC context reference for
 * advanced use cases (e.g. reading metadata, tracing).
 */
template <typename ReqType, typename RspType>
void RegisterServiceFunc(
    ibmw::rpc::ServiceBase& service,
    std::string_view func_name,
    std::function<uint32_t(ibmw::rpc::ContextRef, const ReqType&, RspType&)>&& handler) {
  service.RegisterServiceFunc(
      func_name,
      nullptr,
      ibmw::json::GetJsonMessageTypeSupport<ReqType>(),
      ibmw::json::GetJsonMessageTypeSupport<RspType>(),
      ibmw::rpc::ServiceFunc(
          [handler{std::move(handler)}](
              const ibmw_service_context_base_t* ctx_ptr,
              const void* req_ptr,
              void* rsp_ptr,
              ibmw_function_base_t* callback) {
            auto& req = *static_cast<const ReqType*>(req_ptr);
            auto& rsp = *static_cast<RspType*>(rsp_ptr);

            uint32_t code = handler(ibmw::rpc::ContextRef(ctx_ptr), req, rsp);

            auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(
                callback->ops);
            ops->invoker(callback->object_buf, code);
          }));
}

// ── Client: typed RegisterClientFunc ────────────────────────────────────────

/**
 * @brief Register a JSON RPC client function with automatic type support.
 *
 * @tparam ReqType  Request struct.
 * @tparam RspType  Response struct.
 * @param service_ref  The service reference.
 * @param service_name The service name (e.g. "my_project.ComputeService").
 * @param func_name    The function name (e.g. "Compute").
 * @return true if registration succeeded.
 */
template <typename ReqType, typename RspType>
bool RegisterClientFunc(
    ibmw::rpc::ServiceRef service_ref,
    std::string_view service_name,
    std::string_view func_name) {
  std::string full_func_name =
      ibmw::rpc::GetFullFuncName("json", service_name, func_name);

  return service_ref.RegisterClientFunc(
      full_func_name,
      nullptr,
      ibmw::json::GetJsonMessageTypeSupport<ReqType>(),
      ibmw::json::GetJsonMessageTypeSupport<RspType>());
}

// ── Client: typed synchronous Call ──────────────────────────────────────────

/**
 * @brief Synchronous JSON RPC call with automatic context management.
 *
 * Creates a context, sets serialization type to "json", and uses
 * promise/future internally for synchronous waiting.
 *
 * @tparam ReqType  Request struct.
 * @tparam RspType  Response struct.
 * @param service_ref  The service reference.
 * @param service_name The service name.
 * @param func_name    The function name.
 * @param req          The request message.
 * @param rsp          [out] The response message.
 * @param timeout      Optional timeout (default: 3 seconds).
 * @return RPC status.
 */
template <typename ReqType, typename RspType>
ibmw::rpc::Status Call(
    ibmw::rpc::ServiceRef service_ref,
    std::string_view service_name,
    std::string_view func_name,
    const ReqType& req,
    RspType& rsp,
    std::chrono::nanoseconds timeout = std::chrono::milliseconds(3000)) {
  std::string full_func_name =
      ibmw::rpc::GetFullFuncName("json", service_name, func_name);

  auto ctx_ptr = std::make_shared<ibmw::rpc::Context>();
  ctx_ptr->SetTimeout(timeout);
  ctx_ptr->SetFunctionName(full_func_name);

  auto result = std::make_shared<ibmw::rpc::details::SyncCallResult>();

  service_ref.Call(
      full_func_name, *ctx_ptr, &req, &rsp,
      [ctx_ptr, result](uint32_t code) { result->Set(ibmw::rpc::Status(code)); });

  return result->Get();
}

/**
 * @brief Asynchronous JSON RPC call with callback.
 *
 * @tparam ReqType  Request struct.
 * @tparam RspType  Response struct.
 * @param service_ref  The service reference.
 * @param service_name The service name.
 * @param func_name    The function name.
 * @param req          The request message.
 * @param rsp          [out] The response message (must outlive the callback).
 * @param callback     Called with the RPC status when complete.
 * @param timeout      Optional timeout (default: 3 seconds).
 */
template <typename ReqType, typename RspType>
void CallAsync(
    ibmw::rpc::ServiceRef service_ref,
    std::string_view service_name,
    std::string_view func_name,
    const ReqType& req,
    RspType& rsp,
    std::function<void(ibmw::rpc::Status)>&& callback,
    std::chrono::nanoseconds timeout = std::chrono::milliseconds(3000)) {
  std::string full_func_name =
      ibmw::rpc::GetFullFuncName("json", service_name, func_name);

  auto ctx_ptr = std::make_shared<ibmw::rpc::Context>();
  ctx_ptr->SetTimeout(timeout);
  ctx_ptr->SetFunctionName(full_func_name);

  service_ref.Call(
      full_func_name, *ctx_ptr, &req, &rsp,
      [ctx_ptr, callback{std::move(callback)}](uint32_t code) {
        callback(ibmw::rpc::Status(code));
      });
}

}  // namespace ibmw::json::rpc
