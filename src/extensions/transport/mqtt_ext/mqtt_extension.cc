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
    // Secrets (client_key_password, password) are deliberately not written
    // back: the encoded node is exported with the effective configuration.
    node["callback_executor_name"] = rhs.callback_executor_name;
    node["username"] = rhs.username;
    node["keep_alive_s"] = rhs.keep_alive_s;
    node["clean_session"] = rhs.clean_session;
    node["tls_verify_hostname"] = rhs.tls_verify_hostname;
    node["start_connect_wait_ms"] = rhs.start_connect_wait_ms;

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

    if (node["username"])
      rhs.username = node["username"].as<std::string>();

    if (node["password"])
      rhs.password = node["password"].as<std::string>();

    if (node["keep_alive_s"])
      rhs.keep_alive_s = node["keep_alive_s"].as<uint32_t>();

    if (node["clean_session"])
      rhs.clean_session = node["clean_session"].as<bool>();
    if (node["tls_verify_hostname"])
      rhs.tls_verify_hostname = node["tls_verify_hostname"].as<bool>();
    if (node["start_connect_wait_ms"])
      rhs.start_connect_wait_ms = node["start_connect_wait_ms"].as<uint32_t>();

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

  // Validate the connection options (URL, TLS files) up front so that a
  // misconfiguration fails initialization instead of retrying forever.
  {
    MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
    MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
    BuildConnectOptions(conn_opts, ssl_opts);
  }

  // The catalog must exist before the client can deliver any message.
  msg_handle_catalog_ptr_ = std::make_shared<MsgHandleCatalog>();

  // initialize mqtt
  const int create_rc = MQTTAsync_create(
      &client_, options_.broker_addr.c_str(), options_.client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
  if (create_rc != MQTTASYNC_SUCCESS) {
    client_ = nullptr;
    MW_ERROR("Failed to create mqtt client for '{}', rc: {}", options_.broker_addr, create_rc);
    return false;
  }

  // From here on a failure must not leave a client whose callbacks point
  // at this object (OnShutdown does not run after a failed OnInit).
  try {
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

  // The first connect is made once the engine starts: every message handler
  // is registered by then (the catalog map is not synchronized), and with
  // clean_session=false the broker may deliver queued messages right after
  // CONNACK. Subscriptions are made on connect (and after every reconnect),
  // without blocking Paho callback threads (TRANS-012). Startup waits
  // (bounded) for the outcome of the first attempt, so that modules starting
  // next do not publish into a connection that is not there yet.
  core_ptr_->RegisterHook(runtime::core::BeforePhase(runtime::core::Phase::kStart),
                          [this] {
                            msg_handle_catalog_ptr_->Seal();
                            {
                              std::lock_guard<std::mutex> lock(conn_mutex_);
                              started_ = true;
                            }
                            if (!connect_thread_.joinable())
                              connect_thread_ = std::thread([this] { ConnectLoop(); });
                            RequestConnect(false);
                            WaitFirstConnectAttempt();
                          });

  core_ptr_->RegisterHook(runtime::core::BeforePhase(runtime::core::Phase::kStopping),
                          [this] {
                            std::lock_guard<std::mutex> lock(conn_mutex_);
                            hooks_enabled_ = false;
                          });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);
  } catch (...) {
    MQTTAsync_destroy(&client_);
    client_ = nullptr;
    throw;
  }

  return true;
}

MqttExtension::~MqttExtension() {
  // Normally done by OnShutdown; covers an engine that never shut down.
  if (connect_thread_.joinable()) {
    stop_flag_ = true;
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      hooks_enabled_ = false;
    }
    conn_cv_.notify_all();
    connect_thread_.join();
  }
  if (client_ != nullptr) {
    // Handlers must be idle before the client (and the drivers) go away.
    if (msg_handle_catalog_ptr_) msg_handle_catalog_ptr_->Shutdown();
    MQTTAsync_destroy(&client_);
  }
}

void MqttExtension::OnShutdown() {
  stop_flag_ = true;
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    hooks_enabled_ = false;
  }
  conn_cv_.notify_all();
  if (connect_thread_.joinable()) connect_thread_.join();

  if (msg_handle_catalog_ptr_) msg_handle_catalog_ptr_->Shutdown();

  if (client_ == nullptr) return;

  // Wait (bounded) for the disconnect to complete before destroying the
  // client, instead of destroying it under an in-flight disconnect (TRANS-011).
  // Paho (1.3.x) runs these callbacks with its global mqttasync_mutex held and
  // MQTTAsync_destroy takes that mutex and frees the pending command, so no
  // Done() can run after destroy returns. Re-check this on a Paho upgrade.
  struct DisconnectWait {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    static void Done(void *context) {
      auto *self = static_cast<DisconnectWait *>(context);
      std::lock_guard<std::mutex> lock(self->mutex);
      self->done = true;
      self->cv.notify_all();
    }
  };
  auto wait = std::make_shared<DisconnectWait>();
  MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;
  disc_opts.timeout = 1000;
  disc_opts.onSuccess = [](void *context, MQTTAsync_successData *) { DisconnectWait::Done(context); };
  disc_opts.onFailure = [](void *context, MQTTAsync_failureData *) { DisconnectWait::Done(context); };
  disc_opts.context = wait.get();
  if (MQTTAsync_disconnect(client_, &disc_opts) == MQTTASYNC_SUCCESS) {
    std::unique_lock<std::mutex> lock(wait->mutex);
    if (!wait->cv.wait_for(lock, std::chrono::milliseconds(2000), [&] { return wait->done; })) {
      MW_WARN("Mqtt disconnect did not complete in time.");
    }
  }

  MQTTAsync_destroy(&client_);
  client_ = nullptr;
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

void MqttExtension::BuildConnectOptions(MQTTAsync_connectOptions &conn_opts, MQTTAsync_SSLOptions &ssl_opts) {
  conn_opts.keepAliveInterval = static_cast<int>(options_.keep_alive_s);
  conn_opts.cleansession = options_.clean_session ? 1 : 0;

  if (!options_.username.empty()) {
    conn_opts.username = options_.username.c_str();
    if (!options_.password.empty()) conn_opts.password = options_.password.c_str();
  }

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
    MW_CHECK_WARN(options_.username.empty(), "Mqtt credentials are sent without TLS.");
  }
}

int MqttExtension::StartConnect() {
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
  BuildConnectOptions(conn_opts, ssl_opts);

  conn_opts.onSuccess = [](void *context, MQTTAsync_successData *) {
    static_cast<MqttExtension *>(context)->OnConnected();
  };
  conn_opts.onFailure = [](void *context, MQTTAsync_failureData *response) {
    static_cast<MqttExtension *>(context)->OnConnectFailed(
        (!response || !response->message) ? "Unknown error" : response->message);
  };
  conn_opts.context = this;
  // Paho copies the options (including TLS settings) before returning.
  return MQTTAsync_connect(client_, &conn_opts);
}

void MqttExtension::RequestConnect(bool delayed) {
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    connect_requested_ = true;
    next_attempt_ = std::chrono::steady_clock::now() +
                    (delayed ? std::chrono::milliseconds(options_.reconnect_interval_ms) : std::chrono::milliseconds(0));
  }
  conn_cv_.notify_all();
}

void MqttExtension::WaitFirstConnectAttempt() {
  if (options_.start_connect_wait_ms == 0) return;
  std::unique_lock<std::mutex> lock(conn_mutex_);
  if (!conn_cv_.wait_for(lock, std::chrono::milliseconds(options_.start_connect_wait_ms),
                         [this] { return first_attempt_done_ || stop_flag_.load(); })) {
    MW_WARN("Mqtt broker '{}' did not answer within {} ms; starting without a connection.",
            options_.broker_addr, options_.start_connect_wait_ms);
  }
}

void MqttExtension::ConnectLoop() {
  std::unique_lock<std::mutex> lock(conn_mutex_);
  while (true) {
    conn_cv_.wait(lock, [this] { return stop_flag_.load() || connect_requested_; });
    if (stop_flag_.load()) return;
    if (conn_cv_.wait_until(lock, next_attempt_, [this] { return stop_flag_.load(); })) return;
    connect_requested_ = false;
    lock.unlock();
    int rc = MQTTASYNC_FAILURE;
    try {
      rc = StartConnect();
    } catch (const std::exception &e) {
      MW_ERROR("Failed to build mqtt connect options: {}", e.what());
    }
    if (rc != MQTTASYNC_SUCCESS) {
      MW_ERROR("Failed to start connection, rc: {}", rc);
      MarkAttemptDone();
      RequestConnect(true);
    }
    lock.lock();
  }
}

void MqttExtension::OnConnected() {
  MW_INFO("Connect to mqtt broker success.");
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    connected_ = true;
    first_attempt_done_ = true;
    if (started_) RunReconnectHooksLocked();
  }
  conn_cv_.notify_all();
}

void MqttExtension::OnConnectFailed(const char *message) {
  MW_WARN("Failed to connect mqtt broker: {}", message);
  MarkAttemptDone();
  if (!stop_flag_.load()) RequestConnect(true);
}

void MqttExtension::MarkAttemptDone() {
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    first_attempt_done_ = true;
  }
  conn_cv_.notify_all();
}

void MqttExtension::OnConnectLost(const char *cause) {
  MW_WARN("Lost connect to mqtt broker, cause {}", (cause == nullptr) ? "nil" : cause);
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    connected_ = false;
  }
  if (!stop_flag_.load()) RequestConnect(false);
}

void MqttExtension::RunReconnectHooksLocked() {
  if (!hooks_enabled_) return;
  for (const auto &f : reconnect_hook_) {
    try {
      f();
    } catch (const std::exception &e) {
      MW_ERROR("Mqtt resubscribe failed: {}", e.what());
    }
  }
}

int MqttExtension::OnMsgRecv(char *topic, int topic_len, MQTTAsync_message *message) {
  std::string_view topic_str = topic_len ? std::string_view(topic, topic_len) : std::string_view(topic);
  msg_handle_catalog_ptr_->HandleServerMsg(topic_str, topic, message);
  return 1;
}

void MqttExtension::SetSSL(MQTTAsync_connectOptions &conn_opts, MQTTAsync_SSLOptions &ssl_opts) const {
  // check if set ca file
  MW_REQUIRE(!options_.truststore.empty(), "Use ssl/mqtts must set truststore");
  ssl_opts.trustStore = options_.truststore.c_str();
  // Paho defaults to no host name check: any certificate from the trusted CA
  // would be accepted for any broker.
  ssl_opts.verify = options_.tls_verify_hostname ? 1 : 0;

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
