// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "subscriber_module.h"

namespace ibmw::examples::ros2_interop_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // 1. 获取Subscriber
  auto subscriber = core_.GetTransport().GetSubscriber("chatter");

  // 2. 订阅ROS2消息
  bool success = ibmw::ros2::transport::OnMessage<std_msgs::msg::String>(
    subscriber,
    "std_msgs/msg/String",
    [this](ibmw::transport::ContextRef ctx,
           const std::shared_ptr<const std_msgs::msg::String>& msg) {
      OnMessage(ctx, msg);
    });

  if (!success) {
    MW_ERROR("Failed to subscribe ROS2 type: std_msgs/msg/String");
    return false;
  }

  MW_INFO("SubscriberModule initialized successfully");
  return true;
}

void SubscriberModule::Stop() {
  MW_INFO("SubscriberModule shutdown, total received: {}", receive_count_);
}

void SubscriberModule::OnMessage(
    ibmw::transport::ContextRef ctx,
    const std::shared_ptr<const std_msgs::msg::String>& msg) {

  receive_count_++;

  MW_INFO("Received [{}]: {}", receive_count_, msg->data);

  // 打印Context信息
  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {}", serialization_type);
}

}  // namespace ibmw::examples::ros2_interop_demo
