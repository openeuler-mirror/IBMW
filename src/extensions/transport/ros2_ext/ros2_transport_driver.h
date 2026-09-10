// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_tools.h"

#include "ros2_ext/ros2_adapter_subscription.h"

#include "rclcpp/rclcpp.hpp"
#include "rcpputils/shared_library.hpp"

#include "rcl/publisher.h"

#include "ros2_ext_proto/msg/ros_msg_wrapper.hpp"

namespace ibmw::extensions::ros2_extension {

class Ros2TransportDriver : public runtime::core::transport::TransportDriver {
 public:
  struct Options {
    struct QosOptions {
      std::string history = "default";
      int32_t depth = 10;
      std::string reliability = "default";
      std::string durability = "default";
      int64_t deadline = -1;
      int64_t lifespan = -1;
      std::string liveliness = "default";
      int64_t liveliness_lease_duration = -1;
    };

    struct PubTopicOptions {
      std::string topic_name;
      bool use_serialized = false;
      QosOptions qos;
    };

    std::vector<PubTopicOptions> pub_topics_options;

    struct SubTopicOptions {
      std::string topic_name;
      bool use_serialized = false;
      QosOptions qos;
    };

    std::vector<SubTopicOptions> sub_topics_options;
  };

 public:
  Ros2TransportDriver() = default;
  ~Ros2TransportDriver() override = default;

  std::string_view Name() const noexcept override { return "ros2"; }
  IBMW_DECLARE_DRIVER_SLOT("ros2")

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

  void SetNodePtr(const std::shared_ptr<rclcpp::Node>& ros2_node_ptr) {
    ros2_node_ptr_ = ros2_node_ptr;
  }

 private:
  static bool CheckRosMsg(std::string_view msg_type) {
    return (msg_type.substr(0, 5) == "ros2:");
  }
  rclcpp::QoS GetQos(const Options::QosOptions& qos_option);

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

  std::shared_ptr<rclcpp::Node> ros2_node_ptr_;

  struct Key {
    std::string topic_name;
    std::string msg_type;

    bool operator==(const Key& rhs) const {
      return topic_name == rhs.topic_name && msg_type == rhs.msg_type;
    }

    struct Hash {
      std::size_t operator()(const Key& k) const {
        return (std::hash<std::string>()(k.topic_name)) ^
               (std::hash<std::string>()(k.msg_type));
      }
    };
  };

  struct RosPubWrapper {
    std::unique_ptr<rcl_publisher_t> publisher_ptr;
    bool use_serialized;
    std::shared_ptr<rcpputils::SharedLibrary> type_support_library;
  };

  std::unordered_map<
      Key,
      RosPubWrapper,
      Key::Hash,
      std::equal_to<>>
      ros2_send_type_wrapper_map_;

  struct RosSubWrapper {
    std::unique_ptr<ibmw::runtime::core::transport::MessageHandlerTool> msg_handler_tool_ptr;
    std::shared_ptr<rclcpp::SubscriptionBase> ros_sub_handle_ptr;
    std::shared_ptr<rcpputils::SharedLibrary> type_support_library;
  };

  std::unordered_map<
      Key,
      RosSubWrapper,
      Key::Hash,
      std::equal_to<>>
      ros2_message_handler_map_;

  std::unordered_map<
      std::string,
      rclcpp::Publisher<ros2_ext_proto::msg::RosMsgWrapper>::SharedPtr>
      publisher_map_;

  struct SubWrapper {
    std::unique_ptr<ibmw::runtime::core::transport::MessageHandlerTool> msg_handler_tool_ptr;
    rclcpp::Subscription<ros2_ext_proto::msg::RosMsgWrapper>::SharedPtr ros_sub_handle_ptr;
  };

  std::unordered_map<std::string, SubWrapper> message_handler_map_;
};

}  // namespace ibmw::extensions::ros2_extension
