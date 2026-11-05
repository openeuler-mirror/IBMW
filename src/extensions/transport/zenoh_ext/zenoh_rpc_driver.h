// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_set>

#include "cpp/service/rpc_status.h"
#include "cpp/util/type_support.h"
#include "engine/service/service_driver.h"
#include "extension_api/ibmw_service_client_readiness.h"
#include "engine/service/service_driver_tools.h"
#include "engine/rpc_client_tool.h"
#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "zenoh.h"
#include "zenoh_ext/key_expr.h"
#include "zenoh_ext/util.h"
#include "zenoh_ext/zenoh_buffer_array_allocator.h"
#include "zenoh_ext/zenoh_bus.h"

namespace ibmw::extensions::zenoh_extension {
class ZenohRpcDriver : public runtime::core::service::ServiceDriver,
                       public ibmw::extension::ServiceClientReadinessV1 {
 public:
  struct Options {
    std::string timeout_executor;
    struct ClientOptions {
      std::string func_name;
      bool shm_enabled = false;
    };
    std::vector<ClientOptions> clients_options;

    struct ServerOptions {
      std::string func_name;
      bool shm_enabled = false;
    };
    std::vector<ServerOptions> servers_options;
  };

 public:
  ZenohRpcDriver(
      std::shared_ptr<ZenohBus>& zenoh_bus_ptr, const std::string& limit_domain)
      : zenoh_bus_ptr_(zenoh_bus_ptr),
        limit_domain_(NormalizeLimitDomain(limit_domain)),
        key_suffix_(LimitDomainKeySuffix(limit_domain_)) {}
  ~ZenohRpcDriver();

  std::string_view Name() const noexcept override { return "zenoh"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;
  bool IsClientReady(std::string_view module, std::string_view function) const noexcept override;

  bool RegisterServiceFunc(
      const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept override;
  bool RegisterClientFunc(
      const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept override;
  void Call(
      const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept override;

  void RegisterGetExecutorFunc(const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func);

 private:
  void ReturnRspWithStatusCode(
      const std::string& pattern,
      std::string_view serialization_type,
      const char* req_id_buf,
      uint32_t code);

  enum class State : uint32_t {
    PreInit,
    Init,
    Start,
    Shutdown,
  };

  Options options_;
  std::atomic<State> state_ = State::PreInit;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;

  std::atomic_uint32_t req_id_{runtime::core::util::RandomReqIdStart()};

  std::shared_ptr<ZenohBus> zenoh_bus_ptr_;
  std::string limit_domain_;
  std::string key_suffix_;

  std::unique_ptr<runtime::core::util::RpcClientTool<std::shared_ptr<runtime::core::service::ServiceMessageFrame>>> client_tool_ptr_;

  ShmLoanSizes shm_loan_sizes_;
  std::unordered_set<std::string> sub_keyexprs_;  // bus subscribers of the handlers, removed by Shutdown

  uint64_t shm_init_loan_size_ = 1024;
};

}  // namespace ibmw::extensions::zenoh_extension
