// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>

#include "engine/discovery/service_discovery_protocol.h"
#include "engine/service/service_driver.h"
#include "extension_api/ibmw_service_client_readiness.h"
#include "engine/rpc_client_tool.h"
#include "mqtt_ext/msg_handle_catalog.h"

namespace ibmw::extensions::mqtt_extension {

class MqttRpcDriver : public runtime::core::service::ServiceDriver,
                      public ibmw::extension::ServiceClientReadinessV1 {
 public:
  struct Options {
    std::string timeout_executor;

    struct ClientOptions {
      std::string func_name;
      std::string server_mqtt_id;
      int qos = 2;
    };
    std::vector<ClientOptions> clients_options;

    struct ServerOptions {
      std::string func_name;
      bool allow_share = true;
      int qos = 2;
    };
    std::vector<ServerOptions> servers_options;
  };

 public:
  MqttRpcDriver(
      const std::string& client_id,
      MQTTAsync& client,
      uint32_t max_pkg_size,
      const std::shared_ptr<MsgHandleCatalog>& msg_handle_catalog_ptr)
      : client_id_(client_id),
        client_(client),
        max_pkg_size_(max_pkg_size),
        msg_handle_catalog_ptr_(msg_handle_catalog_ptr) {}

  ~MqttRpcDriver() override = default;

  std::string_view Name() const noexcept override { return "mqtt"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;
  bool IsClientReady(std::string_view module, std::string_view function) const noexcept override;

  void SetServiceCatalog(const runtime::core::service::ServiceCatalog* service_catalog_ptr) noexcept override {
    service_catalog_ptr_ = service_catalog_ptr;
  }

  bool RegisterServiceFunc(
      const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept override;
  bool RegisterClientFunc(
      const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept override;
  void Call(
      const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept override;

  void RegisterGetExecutorFunc(const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func);

  void SubscribeMqttTopic();
  void UnSubscribeMqttTopic();

 private:
  struct SubscriptionStatus {
    std::atomic_size_t pending{0};
    std::atomic_bool failed{false};
  };
  using SubscriptionBatch = std::unordered_map<std::string, std::shared_ptr<SubscriptionStatus>>;
  std::shared_ptr<SubscriptionBatch> subscriptions_;
  void ReturnRspWithStatusCode(
      std::string_view mqtt_pub_topic,
      int qos,
      std::string_view serialization_type,
      const char* req_id_buf,
      uint32_t code);

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  std::shared_ptr<ibmw::discovery::service_protocol::Catalog> discovery_catalog_ =
      std::make_shared<ibmw::discovery::service_protocol::Catalog>();
  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::service::ServiceCatalog* service_catalog_ptr_ = nullptr;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;

  std::string client_id_;
  MQTTAsync& client_;
  uint32_t max_pkg_size_;

  struct MqttSubInfo {
    std::string topic;
    int qos;
  };
  std::vector<MqttSubInfo> sub_info_vec_;

  std::shared_ptr<MsgHandleCatalog> msg_handle_catalog_ptr_;

  std::atomic_uint32_t req_id_ = 0;

  struct ClientCfgInfo {
    std::string server_mqtt_id;
    int qos;
  };
  std::unordered_map<std::string_view, ClientCfgInfo> client_cfg_info_map_;

  std::unique_ptr<runtime::core::util::RpcClientTool<std::shared_ptr<runtime::core::service::ServiceMessageFrame>>>
      client_tool_ptr_;
};

}  // namespace ibmw::extensions::mqtt_extension
