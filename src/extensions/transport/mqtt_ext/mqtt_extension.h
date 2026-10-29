// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "MQTTAsync.h"

#include "engine/extension/extension_base.h"
#include "mqtt_ext/msg_handle_catalog.h"

namespace ibmw::extensions::mqtt_extension {

class MqttExtension : public ExtensionBase {
 public:
  struct Options {
    std::string broker_addr;
    std::string client_id;
    uint32_t max_pkg_size_k = 1024;
    uint32_t reconnect_interval_ms = 1000;
    std::string truststore;
    std::string client_cert;
    std::string client_key;
    std::string client_key_password;
    std::string callback_executor_name;
    std::string username;
    std::string password;
    uint32_t keep_alive_s = 20;
    bool clean_session = true;
    // Check that the broker certificate matches the host name (TLS only).
    bool tls_verify_hostname = true;
    // How long startup waits for the first connect attempt (0: no wait).
    uint32_t start_connect_wait_ms = 3000;
  };

 public:
  MqttExtension();
  ~MqttExtension() override;

  std::string_view Name() const noexcept override { return "mqtt_extension"; }

  bool OnInit(runtime::core::Engine *core_ptr) override;
  void OnShutdown() override;

 private:
  void SetSSL(MQTTAsync_connectOptions &conn_opts, MQTTAsync_SSLOptions &ssl_opts) const;
  void RegisterMqttRpcDriver();
  void RegisterMqttTransportDriver();

  // Connection management (TRANS-006/007/012): every attempt, initial or
  // after a lost connection, is made from connect_thread_ with the same full
  // option set (TLS, credentials). Paho callback threads never sleep, block
  // or recurse; they only record state and wake the thread.
  void BuildConnectOptions(MQTTAsync_connectOptions &conn_opts, MQTTAsync_SSLOptions &ssl_opts);
  int StartConnect();
  void RequestConnect(bool delayed);
  void ConnectLoop();
  void WaitFirstConnectAttempt();
  void MarkAttemptDone();
  void OnConnected();
  void OnConnectFailed(const char *message);
  void OnConnectLost(const char *cause);
  void RunReconnectHooksLocked();
  int OnMsgRecv(char *topic, int topic_len, MQTTAsync_message *message);

 private:
  Options options_;

  std::atomic_bool stop_flag_ = false;

  MQTTAsync client_ = nullptr;
  std::shared_ptr<MsgHandleCatalog> msg_handle_catalog_ptr_;

  std::vector<std::function<void()>> reconnect_hook_;


  std::mutex conn_mutex_;
  std::condition_variable conn_cv_;
  bool connect_requested_ = false;
  std::chrono::steady_clock::time_point next_attempt_;
  bool connected_ = false;
  bool first_attempt_done_ = false;
  bool started_ = false;
  // Hooks call into drivers owned by the transport/service buses, which are
  // destroyed before this extension: disabled when the engine starts stopping.
  bool hooks_enabled_ = true;
  std::thread connect_thread_;
};

}  // namespace ibmw::extensions::mqtt_extension
