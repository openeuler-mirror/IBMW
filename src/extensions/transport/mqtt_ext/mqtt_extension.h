// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "MQTTAsync.h"

#include "engine/extension/extension_base.h"
#include "mqtt_ext/msg_handle_catalog.h"
#include "base/light_signal.h"

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
  };

 public:
  MqttExtension();
  ~MqttExtension() override = default;

  std::string_view Name() const noexcept override { return "mqtt_extension"; }

  bool OnInit(runtime::core::Engine *core_ptr) override;
  void OnShutdown() override;

 private:
  void SetSSL(MQTTAsync_connectOptions &conn_opts, MQTTAsync_SSLOptions &ssl_opts) const;
  void RegisterMqttRpcDriver();
  void RegisterMqttTransportDriver();

  void AsyncConnect();
  void OnConnectLost(const char *cause);
  int OnMsgRecv(char *topic, int topic_len, MQTTAsync_message *message);

 private:
  Options options_;

  std::atomic_bool stop_flag_ = false;

  MQTTAsync client_;
  std::shared_ptr<MsgHandleCatalog> msg_handle_catalog_ptr_;

  std::vector<std::function<void()>> reconnect_hook_;

  common::util::LightSignal signal_;
};

}  // namespace ibmw::extensions::mqtt_extension
