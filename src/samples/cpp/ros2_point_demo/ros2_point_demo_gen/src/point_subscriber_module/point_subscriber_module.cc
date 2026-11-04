// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "point_subscriber_module/point_subscriber_module.h"

#include <cstdio>

namespace ros2_point_demo {

bool PointSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Get subscriber handle
  auto subscriber = core_.GetTransport().GetSubscriber("point_topic");

  // Subscribe to ROS2 messages with context callback
  bool success = ibmw::ros2::transport::OnMessage<geometry_msgs::msg::Point>(
      subscriber,
      "geometry_msgs/msg/Point",
      [this](ibmw::transport::ContextRef ctx,
             const std::shared_ptr<const geometry_msgs::msg::Point>& msg) {
        OnMessage(ctx, msg);
      });

  if (!success) {
    MW_ERROR("Failed to subscribe ROS2 type: geometry_msgs/msg/Point");
    return false;
  }

  MW_INFO("PointSubscriberModule::Setup succeeded.");
  return true;
}

void PointSubscriberModule::Stop() {
  MW_INFO("PointSubscriberModule::Stop, total received: {}", receive_count_);
}

void PointSubscriberModule::OnMessage(
    ibmw::transport::ContextRef ctx,
    const std::shared_ptr<const geometry_msgs::msg::Point>& msg) {

  receive_count_++;

  // Synchronous harness evidence: callback-thread MW_INFO can be buffered by
  // the logger while the live interop test is already checking process logs.
  std::fprintf(stderr, "Received ROS2 interop message [%u]\n", receive_count_);
  MW_INFO("Received ROS2 interop message [{}]", receive_count_);
  // IBMW: user code begin (on_message_body)
  MW_INFO("Received point: x={}, y={}, z={}", msg->x, msg->y, msg->z);
  // IBMW: user code end (on_message_body)

  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {}", serialization_type);
}

}  // namespace ros2_point_demo
