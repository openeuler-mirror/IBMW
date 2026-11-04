// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <cstdio>

namespace ros2_interop_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Get subscriber handle
  auto subscriber = core_.GetTransport().GetSubscriber("chatter");

  // Subscribe to ROS2 messages with context callback
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

  MW_INFO("SubscriberModule::Setup succeeded.");
  return true;
}

void SubscriberModule::Stop() {
  MW_INFO("SubscriberModule::Stop, total received: {}", receive_count_);
}

void SubscriberModule::OnMessage(
    ibmw::transport::ContextRef ctx,
    const std::shared_ptr<const std_msgs::msg::String>& msg) {

  receive_count_++;

  // Synchronous harness evidence: callback-thread MW_INFO can be buffered by
  // the logger while the live interop test is already checking process logs.
  std::fprintf(stderr, "Received ROS2 interop message [%u]\n", receive_count_);
  MW_INFO("Received ROS2 interop message [{}]", receive_count_);
  // IBMW: user code begin (on_message_body)
  MW_INFO("Received data: '{}'", msg->data);
  // IBMW: user code end (on_message_body)

  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {}", serialization_type);
}

}  // namespace ros2_interop_demo
