// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/service/inprocess_service.h"

#include <optional>

#include "engine/service/service_driver_tools.h"
#include "base/string_util.h"
#include "base/url_parser.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::service::InProcessServiceDriver::Options> {
  using Options = ibmw::runtime::core::service::InProcessServiceDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["timeout_executor"] = rhs.timeout_executor;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["timeout_executor"])
      rhs.timeout_executor = node["timeout_executor"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::service {

void InProcessServiceDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Local service driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  cb_ctx_ptr_ = std::make_shared<CallbackContext>();
  cb_ctx_ptr_->logger_ptr = logger_ptr_;

  if (!options_.timeout_executor.empty()) {
    MW_REQUIRE(
        get_executor_func_,
        "Get executor function is not set before initialize.");

    auto timeout_executor = get_executor_func_(options_.timeout_executor);

    MW_REQUIRE(
        timeout_executor,
        "Get timeout executor '{}' failed.", options_.timeout_executor);

    cb_ctx_ptr_->client_tool.RegisterTimeoutExecutor(timeout_executor);
    cb_ctx_ptr_->client_tool.RegisterTimeoutHandle(
        [](std::shared_ptr<ServiceMessageFrame>&& client_call_wrapper_ptr) {
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_TIMEOUT));
        });

    MW_TRACE("Local service driver enable the timeout function, use '{}' as timeout executor.",
                options_.timeout_executor);
  } else {
    // Without a timeout executor a pending call is only completed by the
    // server's answer: a server that never answers keeps the caller waiting.
    MW_WARN("Local service driver has no 'timeout_executor': RPC timeouts are not enforced.");
  }

  options_node = options_;
}

void InProcessServiceDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void InProcessServiceDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  service_func_register_index_.clear();

  // cb_ctx_ptr_ is deliberately kept until the driver is destroyed: a Call
  // racing with Shutdown may still read it. Pending callbacks hold their own
  // reference, so late answers stay safe after the driver is gone.

  get_executor_func_ = nullptr;
}

bool InProcessServiceDriver::RegisterServiceFunc(
    const ServiceFuncWrapper& service_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Service func can only be registered when state is 'Init'.");
      return false;
    }

    const auto& info = service_func_wrapper.info;

    service_func_register_index_[info.func_name][info.pkg_path].emplace(info.module_name);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool InProcessServiceDriver::RegisterClientFunc(
    const ClientFuncWrapper& client_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Client func can only be registered when state is 'Init'.");
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void InProcessServiceDriver::Call(
    const std::shared_ptr<ServiceMessageFrame>& client_call_wrapper_ptr) noexcept {
  // Once the call is recorded, every failure completion must claim the
  // record first, so that the result is delivered exactly once whichever of
  // the server answer, the timeout or the failure path gets there first
  // (COMM-015).
  // cb_ctx_ptr_ lives as long as the driver, so a raw pointer is enough here.
  CallbackContext* cb_ctx = nullptr;
  std::optional<uint32_t> recorded_req_id;
  // Also used from the catch blocks of this noexcept function: a throwing
  // client callback must not terminate the process.
  auto fail = [&](uint32_t code) noexcept {
    try {
      if (!recorded_req_id) {
        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(code));
        return;
      }
      if (auto record = cb_ctx->client_tool.GetRecord(*recorded_req_id))
        CallResultCallback(**record, ibmw::rpc::Status(code));
    } catch (...) {
      MW_ERROR("Client callback threw while reporting a failed call.");
    }
  };

  try {
    if (state_.load() != State::kStart) [[unlikely]] {
      MW_WARN("Method can only be called when state is 'Start'.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }
    cb_ctx = cb_ctx_ptr_.get();

    // Check ctx
    std::string_view service_pkg_path, service_module_name;

    auto to_addr = client_call_wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR);

    // url: local://rpc/func_name?pkg_path=xxxx&module_name=yyyy
    if (!to_addr.empty()) {
      namespace util = ibmw::common::util;
      auto url = util::ParseUrl<std::string_view>(to_addr);
      if (url) {
        if (url->protocol != Name()) [[unlikely]] {
          MW_WARN("Invalid addr: {}", to_addr);
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
          return;
        }
        service_pkg_path = util::GetValueFromStrKV(url->query, "pkg_path");
        service_module_name = util::GetValueFromStrKV(url->query, "module_name");
      }
    }

    const auto& client_info = client_call_wrapper_ptr->info;

    // Find qualified conditions in the local service catalog
    auto service_func_register_index_find_func_itr = service_func_register_index_.find(client_info.func_name);
    if (service_func_register_index_find_func_itr == service_func_register_index_.end()) [[unlikely]] {
      MW_ERROR("Service func '{}' is not registered in local service driver.", client_info.func_name);
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_FOUND));
      return;
    }

    if (service_pkg_path.empty()) {
      if (service_module_name.empty()) {
        // Neither pkg nor module were specified, just find the first one
        auto service_func_register_index_find_pkg_itr =
            service_func_register_index_find_func_itr->second.begin();

        service_pkg_path = service_func_register_index_find_pkg_itr->first;
        service_module_name = *(service_func_register_index_find_pkg_itr->second.begin());
      } else {
        // pkg is not specified, but module is specified. Iterate through all pkgs and find the first module that meets the criteria
        for (const auto& itr : service_func_register_index_find_func_itr->second) {
          if (itr.second.find(service_module_name) != itr.second.end()) {
            service_pkg_path = itr.first;
            break;
          }
        }
        if (service_pkg_path.empty()) {
          MW_WARN("Can not find service func '{}' in module '{}'. Addr: {}",
                     client_info.func_name, service_module_name, to_addr);

          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_ADDR));
          return;
        }
      }

    } else {
      auto service_func_register_index_find_pkg_itr =
          service_func_register_index_find_func_itr->second.find(service_pkg_path);

      if (service_func_register_index_find_pkg_itr ==
          service_func_register_index_find_func_itr->second.end()) {
        MW_WARN("Can not find service func '{}' in pkg '{}'. Addr: {}",
                   client_info.func_name, service_pkg_path, to_addr);

        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_ADDR));
        return;
      }

      if (service_module_name.empty()) {
        service_module_name = *(service_func_register_index_find_pkg_itr->second.begin());
      } else {
        auto service_func_register_index_find_module_itr =
            service_func_register_index_find_pkg_itr->second.find(service_module_name);

        if (service_func_register_index_find_module_itr ==
            service_func_register_index_find_pkg_itr->second.end()) {
          MW_WARN("Can not find service func '{}' in pkg '{}' module '{}'. Addr: {}",
                     client_info.func_name, service_pkg_path, service_module_name, to_addr);

          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_ADDR));
          return;
        }
      }
    }

    MW_TRACE("Invoke service func '{}' in pkg '{}' module '{}'.",
                client_info.func_name, service_pkg_path, service_module_name);

    // Find a registered service method
    const auto* service_func_wrapper_ptr =
        service_catalog_ptr_->GetServiceFuncWrapperPtr(client_info.func_name, service_pkg_path, service_module_name);

    if (service_func_wrapper_ptr == nullptr) [[unlikely]] {
      MW_WARN("Can not find service func '{}' in pkg '{}' module '{}'",
                 client_info.func_name, service_pkg_path, service_module_name);

      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    // Create a service invoke wrapper
    auto service_invoke_wrapper_ptr = std::make_shared<ServiceMessageFrame>(ServiceMessageFrame{
        .info = service_func_wrapper_ptr->info});
    const auto& service_info = service_invoke_wrapper_ptr->info;

    // Create service ctx
    auto ctx_ptr = std::make_shared<ibmw::rpc::Context>(ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT);
    service_invoke_wrapper_ptr->ctx_ref = ctx_ptr;

    ctx_ptr->SetTimeout(client_call_wrapper_ptr->ctx_ref.Timeout());

    std::vector<std::pair<std::string, std::string>> meta_snapshot;
    for (const auto& item : client_call_wrapper_ptr->ctx_ref.GetMetaKeys())
      meta_snapshot.emplace_back(item, client_call_wrapper_ptr->ctx_ref.GetMetaValue(item));
    for (const auto& [key, value] : meta_snapshot)
      ctx_ptr->SetMetaValue(key, value);

    ctx_ptr->SetFunctionName(service_info.func_name);
    ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER, Name());
    ctx_ptr->SetMetaValue("ibmw-from_pkg", client_info.pkg_path);
    ctx_ptr->SetMetaValue("ibmw-from_module", client_info.module_name);

    // In the same pkg, or type support doesn't support serialization (e.g. ROS2 types),
    // call directly without serialization using copy-based delivery.
    const bool can_serialize =
        client_info.req_type_support_ref.SerializationTypesSupportedNum() > 0 &&
        client_info.rsp_type_support_ref.SerializationTypesSupportedNum() > 0;

    if (service_pkg_path == client_info.pkg_path || !can_serialize) {
      service_invoke_wrapper_ptr->req_ptr = client_call_wrapper_ptr->req_ptr;
      service_invoke_wrapper_ptr->rsp_ptr = client_call_wrapper_ptr->rsp_ptr;

      // Record the request; it is timeout-protected only when a
      // timeout_executor is configured (see Initialize()).
      auto timeout = client_call_wrapper_ptr->ctx_ref.Timeout();
      uint32_t cur_req_id = req_id_++;
      auto record_ptr = client_call_wrapper_ptr;

      bool ret = cb_ctx->client_tool.Record(cur_req_id, timeout, std::move(record_ptr));
      if (!ret) [[unlikely]] {
        MW_ERROR("Failed to record msg.");
        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
        return;
      }
      recorded_req_id = cur_req_id;

      // Note: ctx_ptr is captured for lifetime — the server may access the
      // context asynchronously.  service_invoke_wrapper_ptr is NOT captured
      // to avoid a shared_ptr circular reference (the server holds it alive
      // via the shared_ptr passed to service_func).
      service_invoke_wrapper_ptr->callback =
          [cb_ctx_ptr{cb_ctx_ptr_}, ctx_ptr, cur_req_id](ibmw::rpc::Status status) {
            cb_ctx_ptr->OnDirectResult(cur_req_id, status);
          };

      service_func_wrapper_ptr->service_func(service_invoke_wrapper_ptr);
      return;
    }

    // Not within a pkg, it needs to be serialized and enable timeout function

    // Record request
    auto timeout = client_call_wrapper_ptr->ctx_ref.Timeout();
    uint32_t cur_req_id = req_id_++;
    auto record_ptr = client_call_wrapper_ptr;

    bool ret = cb_ctx->client_tool.Record(cur_req_id, timeout, std::move(record_ptr));

    if (!ret) [[unlikely]] {
      // It is unlikely to appear generally
      MW_ERROR("Failed to record msg.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }
    recorded_req_id = cur_req_id;

    // client req serialization
    std::string serialization_type(client_call_wrapper_ptr->ctx_ref.GetSerializationType());

    auto buffer_array_view_ptr = TrySerializeReqWithCache(*client_call_wrapper_ptr, serialization_type);
    if (!buffer_array_view_ptr) [[unlikely]] {
      // Serialization failed
      MW_ERROR(
          "Req serialization failed in local service driver, serialization_type {}, pkg_path: {}, module_name: {}, func_name: {}",
          serialization_type, client_info.pkg_path, client_info.module_name, client_info.func_name);

      fail(IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED);
      return;
    }

    // service req deserialization
    std::shared_ptr<void> service_req_ptr = service_info.req_type_support_ref.CreateSharedPtr();
    service_invoke_wrapper_ptr->req_ptr = service_req_ptr.get();

    bool deserialize_ret = service_info.req_type_support_ref.Deserialize(
        serialization_type,
        *(buffer_array_view_ptr->NativeHandle()),
        service_req_ptr.get());

    if (!deserialize_ret) [[unlikely]] {
      // Deserialization failed
      MW_FATAL(
          "Rsp deserialization failed in local service driver, serialization_type {}, pkg_path: {}, module_name: {}, func_name: {}",
          serialization_type, service_info.pkg_path, service_info.module_name, service_info.func_name);

      fail(IBMW_RPC_STATUS_CLI_DESERIALIZATION_FAILED);
      return;
    }

    // cache service req deserialization buf
    service_invoke_wrapper_ptr->req_serialization_cache.emplace(serialization_type, buffer_array_view_ptr);

    // Create service rsp
    std::shared_ptr<void> service_rsp_ptr = service_info.rsp_type_support_ref.CreateSharedPtr();
    service_invoke_wrapper_ptr->rsp_ptr = service_rsp_ptr.get();

    // Set callback.
    // Use weak_ptr for service_invoke_wrapper_ptr to avoid a circular
    // reference: the ServiceMessageFrame's .callback captures a shared_ptr to
    // itself.  If the server never invokes the callback, the cycle would
    // leak.  With weak_ptr, the message frame is freed when the server drops it.
    std::weak_ptr<ServiceMessageFrame> service_invoke_weak = service_invoke_wrapper_ptr;
    service_invoke_wrapper_ptr->callback =
        [cb_ctx_ptr{cb_ctx_ptr_},
         service_invoke_weak,
         ctx_ptr,
         cur_req_id,
         service_req_ptr,
         service_rsp_ptr,
         serialization_type{std::move(serialization_type)}](ibmw::rpc::Status status) {
          cb_ctx_ptr->OnSerializedResult(cur_req_id, service_invoke_weak, serialization_type, status);
        };

    // Call service rpc
    service_func_wrapper_ptr->service_func(service_invoke_wrapper_ptr);
  } catch (const std::exception& e) {
    // Includes exceptions thrown by service_func itself, which may still own
    // the answer callback: fail() only completes if it wins the record.
    MW_ERROR("Local service call failed: {}", e.what());
    fail(IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR);
  } catch (...) {
    MW_ERROR("Local service call failed with a non-std exception.");
    fail(IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR);
  }
}

void InProcessServiceDriver::CallbackContext::OnDirectResult(uint32_t req_id, ibmw::rpc::Status status) {
  auto msg_recorder = client_tool.GetRecord(req_id);
  if (!msg_recorder) [[unlikely]] {
    MW_TRACE("Can not get req id {} from recorder (timeout or duplicate).", req_id);
    return;
  }
  auto client_call_wrapper_ptr = std::move(*msg_recorder);
  CallResultCallback(*client_call_wrapper_ptr, status);
}

void InProcessServiceDriver::CallbackContext::OnSerializedResult(
    uint32_t cur_req_id,
    const std::weak_ptr<ServiceMessageFrame>& service_invoke_weak,
    const std::string& serialization_type,
    ibmw::rpc::Status status) {
  auto msg_recorder = client_tool.GetRecord(cur_req_id);
  if (!msg_recorder) [[unlikely]] {
    // No record is found, which means that the call has timed out. The record has been deleted after the timeout process is gone.
    MW_TRACE("Can not get req id {} from recorder.", cur_req_id);
    return;
  }

  auto service_invoke_wrapper_ptr = service_invoke_weak.lock();
  if (!service_invoke_wrapper_ptr) [[unlikely]] {
    MW_ERROR("Service message frame expired before callback for req id {}.", cur_req_id);
    auto client_call_wrapper_ptr = std::move(*msg_recorder);
    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR));
    return;
  }

  // Get the record
  auto client_call_wrapper_ptr = std::move(*msg_recorder);
  const auto& client_info = client_call_wrapper_ptr->info;

  const auto& service_info = service_invoke_wrapper_ptr->info;

  // service rsp serialization
  auto buffer_array_view_ptr = TrySerializeRspWithCache(*service_invoke_wrapper_ptr, serialization_type);
  if (!buffer_array_view_ptr) [[unlikely]] {
    // Serialization failed
    MW_ERROR(
        "Rsp serialization failed in local service driver, serialization_type {}, pkg_path: {}, module_name: {}, func_name: {}",
        serialization_type, service_info.pkg_path, service_info.module_name, service_info.func_name);

    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED));

    return;
  }

  // client rsp deserialization
  bool deserialize_ret = client_info.rsp_type_support_ref.Deserialize(
      serialization_type,
      *(buffer_array_view_ptr->NativeHandle()),
      client_call_wrapper_ptr->rsp_ptr);

  if (!deserialize_ret) {
    // Deserialization failed
    MW_ERROR(
        "Req deserialization failed in local service driver, serialization_type {}, pkg_path: {}, module_name: {}, func_name: {}",
        serialization_type, client_info.pkg_path, client_info.module_name, client_info.func_name);

    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_DESERIALIZATION_FAILED));

    return;
  }

  // cache client rsp deserialization buf
  client_call_wrapper_ptr->rsp_serialization_cache.emplace(serialization_type, buffer_array_view_ptr);

  // Calling callback
  CallResultCallback(*client_call_wrapper_ptr, status);
}

void InProcessServiceDriver::RegisterGetExecutorFunc(
    const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  get_executor_func_ = get_executor_func;
}

}  // namespace ibmw::runtime::core::service
