// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <utility>

#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_tools.h"
#include "mqtt_ext/msg_handle_catalog.h"

namespace ibmw::extensions::mqtt_extension {

class MqttTransportDriver : public runtime::core::transport::TransportDriver {
 public:
  struct Options {
    struct PubTopicOptions {
      std::string topic_name;
      int qos = 2;
    };

    std::vector<PubTopicOptions> pub_topics_options;

    struct SubTopicOptions {
      std::string topic_name;
      int qos = 2;
    };

    std::vector<SubTopicOptions> sub_topics_options;
  };

 public:
  MqttTransportDriver(
      MQTTAsync& client,
      uint32_t max_pkg_size,
      const std::shared_ptr<MsgHandleCatalog>& msg_handle_catalog_ptr)
      : client_(client),
        max_pkg_size_(max_pkg_size),
        msg_handle_catalog_ptr_(msg_handle_catalog_ptr) {}

  ~MqttTransportDriver() override = default;

  std::string_view Name() const noexcept override { return "mqtt"; }
  IBMW_DECLARE_DRIVER_SLOT("ibmw.transport.mqtt")

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void SetTransportCatalog(const runtime::core::transport::TransportCatalog* transport_catalog_ptr) noexcept override {
    transport_catalog_ptr_ = transport_catalog_ptr;
  }

  bool RegisterSendType(
      const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept override;
  bool OnMessage(const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept override;
  void Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept override;

  void SubscribeMqttTopic();
  void UnSubscribeMqttTopic();

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::transport::TransportCatalog* transport_catalog_ptr_ = nullptr;

  MQTTAsync& client_;
  uint32_t max_pkg_size_;
  std::shared_ptr<MsgHandleCatalog> msg_handle_catalog_ptr_;

  struct MqttSubInfo {
    std::string topic;
    int qos;
  };
  std::vector<MqttSubInfo> sub_info_vec_;

  std::unordered_map<
      std::string,
      std::unique_ptr<ibmw::runtime::core::transport::MessageHandlerTool>>
      message_handler_map_;

  struct PubCfgInfo {
    int qos;
  };
  std::unordered_map<std::string_view, PubCfgInfo> pub_cfg_info_map_;
};

}  // namespace ibmw::extensions::mqtt_extension
