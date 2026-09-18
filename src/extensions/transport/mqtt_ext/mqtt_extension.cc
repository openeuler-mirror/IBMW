// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "mqtt_ext/mqtt_extension.h"

#include "engine/engine.h"
#include "mqtt_ext/global.h"
#include "mqtt_ext/mqtt_transport_driver.h"
#include "mqtt_ext/mqtt_rpc_driver.h"
#include "base/url_parser.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::mqtt_extension::MqttExtension::Options> {
  using Options = ibmw::extensions::mqtt_extension::MqttExtension::Options;

  static Node encode(const Options &rhs) {
    Node node;

    node["broker_addr"] = rhs.broker_addr;
    node["client_id"] = rhs.client_id;
    node["max_pkg_size_k"] = rhs.max_pkg_size_k;
    node["reconnect_interval_ms"] = rhs.reconnect_interval_ms;
    node["truststore"] = rhs.truststore;
    node["client_cert"] = rhs.client_cert;
    node["client_key"] = rhs.client_key;
    node["client_key_password"] = rhs.client_key_password;
    node["callback_executor_name"] = rhs.callback_executor_name;

    return node;
  }

  static bool decode(const Node &node, Options &rhs) {
    if (!node.IsMap()) return false;

    rhs.broker_addr = node["broker_addr"].as<std::string>();
    rhs.client_id = node["client_id"].as<std::string>();

    if (node["max_pkg_size_k"])
      rhs.max_pkg_size_k = node["max_pkg_size_k"].as<uint32_t>();

    if (node["truststore"])
      rhs.truststore = node["truststore"].as<std::string>();

    if (node["client_cert"])
      rhs.client_cert = node["client_cert"].as<std::string>();

    if (node["client_key"])
      rhs.client_key = node["client_key"].as<std::string>();

    if (node["client_key_password"])
      rhs.client_key_password = node["client_key_password"].as<std::string>();

    if (node["reconnect_interval_ms"])
      rhs.reconnect_interval_ms = node["reconnect_interval_ms"].as<uint32_t>();

    if (node["callback_executor_name"])
      rhs.callback_executor_name = node["callback_executor_name"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::mqtt_extension {

MqttExtension::MqttExtension() {
  SetExtensionLogger(&g_logger);
}

bool MqttExtension::OnInit(runtime::core::Engine *core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  // initialize mqtt
  MQTTAsync_create(
      &client_, options_.broker_addr.c_str(), options_.client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);

  MQTTAsync_setCallbacks(
      client_,
      this,
      [](void *context, char *cause) {
        static_cast<MqttExtension *>(context)->OnConnectLost(cause);
      },
      [](void *context, char *topicName, int topicLen, MQTTAsync_message *message) -> int {
        return static_cast<MqttExtension *>(context)->OnMsgRecv(topicName, topicLen, message);
      },
      NULL);

  // connect to broker, which is an async operation
  AsyncConnect();

  msg_handle_catalog_ptr_ = std::make_shared<MsgHandleCatalog>();

  msg_handle_catalog_ptr_->RegisterGetExecutorFunc([this](std::string_view name) -> ibmw::executor::ExecutorHandle {
    return core_ptr_->GetExecutorBus().GetExecutor(name);
  });
  core_ptr_->RegisterHook(runtime::core::After(runtime::core::Phase::kInitialize,
                                                runtime::core::Subsystem::kExecutor),
                          [this] {
                            if(options_.callback_executor_name.empty()) return;
                            msg_handle_catalog_ptr_->InitExecutor(options_.callback_executor_name); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kService),
                          [this] { RegisterMqttRpcDriver(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kTransport),
                          [this] { RegisterMqttTransportDriver(); });

  core_ptr_->RegisterHook(runtime::core::BeforePhase(runtime::core::Phase::kStart),
                          [this] { signal_.Notify(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void MqttExtension::OnShutdown() {
  stop_flag_ = true;

  msg_handle_catalog_ptr_->Shutdown();

  MQTTAsync_disconnect(client_, NULL);

  MQTTAsync_destroy(&client_);
}

void MqttExtension::RegisterMqttTransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> mqtt_transport_driver_ptr =
      std::make_unique<MqttTransportDriver>(
          client_,
          options_.max_pkg_size_k * 1024,
          msg_handle_catalog_ptr_);

  reconnect_hook_.emplace_back(
      [ptr = static_cast<MqttTransportDriver *>(mqtt_transport_driver_ptr.get())]() {
        ptr->SubscribeMqttTopic();
      });
  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(mqtt_transport_driver_ptr), {},
      runtime::core::transport::SerializedTransportCapabilityV2{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kCachedSerialized,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .serialization = runtime::core::transport::SerializedFormatSupport::kTypeSupport,
          .schema = runtime::core::transport::SerializedSchemaSupport::kTypeSupportPackage,
          .loan = runtime::core::transport::SerializedLoanSupport::kSharedPayload,
          .callback_thread = runtime::core::transport::SerializedCallbackThread::kTransportManaged,
          .capture_priority = 3,
          .provider_name = Name(),
          .provider_instance_option = "client_id",
          .provider_clear_option = "callback_executor_name"});
}

void MqttExtension::RegisterMqttRpcDriver() {
  std::unique_ptr<runtime::core::service::ServiceDriver> mqtt_rpc_driver_ptr =
      std::make_unique<MqttRpcDriver>(
          options_.client_id, client_,
          options_.max_pkg_size_k * 1024,
          msg_handle_catalog_ptr_);

  static_cast<MqttRpcDriver *>(mqtt_rpc_driver_ptr.get())
      ->RegisterGetExecutorFunc(
          [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
            return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
          });

  reconnect_hook_.emplace_back(
      [ptr = static_cast<MqttRpcDriver *>(mqtt_rpc_driver_ptr.get())]() {
        ptr->SubscribeMqttTopic();
      });
  core_ptr_->GetServiceDispatcher().RegisterRpcDriver(std::move(mqtt_rpc_driver_ptr));
}

void MqttExtension::AsyncConnect() {
  if (stop_flag_.load()) return;

  // connect to broker
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;

  conn_opts.keepAliveInterval = 20;
  conn_opts.cleansession = 1;

  // check broker_add protocol is ssl/mqtts or not
  auto ret = common::util::ParseUrl(options_.broker_addr);
  MW_REQUIRE(ret != std::nullopt, "Parse broker_addr failed");

  // check if need to set ssl options
  if (ret->protocol == "ssl" || ret->protocol == "mqtts") {
    SetSSL(conn_opts, ssl_opts);
  } else {
    MW_CHECK_WARN(options_.truststore.empty(), "Broker protocol is not ssl/mqtts, the truststore you set will be ignored.");
    MW_CHECK_WARN(options_.client_cert.empty(), "Broker protocol is not ssl/mqtts, the client_cert you set will be ignored.");
    MW_CHECK_WARN(options_.client_key.empty(), "Broker protocol is not ssl/mqtts, the client_key you set will be ignored.");
    MW_CHECK_WARN(options_.client_key_password.empty(), "Broker protocol is not ssl/mqtts, the client_key_password you set will be ignored.");
  }

  // if connect success, call all registered hook functions to subscribe mqtt topic
  conn_opts.onSuccess = [](void *context, MQTTAsync_successData *response) {
    MW_INFO("Connect to mqtt broker success.");
    auto *mqtt_extension_ptr = static_cast<MqttExtension *>(context);

    mqtt_extension_ptr->signal_.Wait();
    mqtt_extension_ptr->signal_.Reset();

    for (const auto &f : mqtt_extension_ptr->reconnect_hook_)
      f();
  };

  // if connect failed, call connect again
  conn_opts.onFailure = [](void *context, MQTTAsync_failureData *response) {
    MW_WARN("Failed to connect mqtt broker: {}", (!response || !response->message) ? "Unknown error" : response->message);
    auto *mqtt_extension_ptr = static_cast<MqttExtension *>(context);
    std::this_thread::sleep_for(std::chrono::milliseconds(mqtt_extension_ptr->options_.reconnect_interval_ms));
    mqtt_extension_ptr->AsyncConnect();
  };
  conn_opts.context = this;
  int rc = MQTTAsync_connect(client_, &conn_opts);

  if (rc != MQTTASYNC_SUCCESS) {
    MW_ERROR("Failed to start connection, rc: {}", rc);
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.reconnect_interval_ms));
    AsyncConnect();  // todo: avoid stack overflow
  }
}

void MqttExtension::OnConnectLost(const char *cause) {
  MW_WARN("Lost connect to mqtt broker, cause {}", (cause == nullptr) ? "nil" : cause);

  if (stop_flag_.load()) return;

  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  conn_opts.keepAliveInterval = 20;
  conn_opts.cleansession = 1;
  conn_opts.onSuccess = [](void *context, MQTTAsync_successData *response) {
    MW_INFO("Reconnect to mqtt broker success.");

    auto *mqtt_extension_ptr = static_cast<MqttExtension *>(context);

    for (const auto &f : mqtt_extension_ptr->reconnect_hook_)
      f();
  };
  conn_opts.onFailure = [](void *context, MQTTAsync_failureData *response) {
    auto *mqtt_extension_ptr = static_cast<MqttExtension *>(context);
    std::this_thread::sleep_for(std::chrono::milliseconds(mqtt_extension_ptr->options_.reconnect_interval_ms));
    mqtt_extension_ptr->OnConnectLost("Reconnect failed");
  };
  conn_opts.context = this;
  int rc = MQTTAsync_connect(client_, &conn_opts);

  if (rc != MQTTASYNC_SUCCESS) {
    MW_ERROR("Failed to reconnect mqtt broker, return code: {}", rc);
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.reconnect_interval_ms));
    OnConnectLost("Reconnect failed");  // TODO: Need to prevent stack overflow
  }
}

int MqttExtension::OnMsgRecv(char *topic, int topic_len, MQTTAsync_message *message) {
  std::string_view topic_str = topic_len ? std::string_view(topic, topic_len) : std::string_view(topic);
  msg_handle_catalog_ptr_->HandleServerMsg(topic_str, message);

  if (options_.callback_executor_name.empty()) {
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topic);
    return 1;
  }

  return 1;
}

void MqttExtension::SetSSL(MQTTAsync_connectOptions &conn_opts, MQTTAsync_SSLOptions &ssl_opts) const {
  // check if set ca file
  MW_REQUIRE(!options_.truststore.empty(), "Use ssl/mqtts must set truststore");
  ssl_opts.trustStore = options_.truststore.c_str();

  bool has_cert = !options_.client_cert.empty();
  bool has_key = !options_.client_key.empty();

  // client_cert and client_key must be set together ,which means use double authentication
  if (has_cert || has_key) {
    MW_REQUIRE(has_cert && has_key,
                            "When using client certificate authentication, both cert_path and key_path must be set");

    // set client certificate and key
    ssl_opts.keyStore = options_.client_cert.c_str();
    ssl_opts.privateKey = options_.client_key.c_str();

    // set client key password
    if (!options_.client_key_password.empty()) {
      ssl_opts.privateKeyPassword = options_.client_key_password.c_str();
    }
  }
  conn_opts.ssl = &ssl_opts;
}

}  // namespace ibmw::extensions::mqtt_extension

IBMW_EXT_ENTRY(ibmw::extensions::mqtt_extension::MqttExtension)
