// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "point_subscriber_module.h"

namespace ibmw::examples::ros2_point_demo {

bool PointSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // 1. 获取Subscriber
  auto subscriber = core_.GetTransport().GetSubscriber("point_topic");

  // 2. 订阅ROS2消息 (geometry_msgs::msg::Point - Plain类型)
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

  MW_INFO("PointSubscriberModule initialized successfully");
  MW_INFO("Subscribing to Plain type: geometry_msgs/msg/Point");
  return true;
}

void PointSubscriberModule::Stop() {
  MW_INFO("PointSubscriberModule shutdown, total received: {}", receive_count_.load());
}

void PointSubscriberModule::OnMessage(
    ibmw::transport::ContextRef ctx,
    const std::shared_ptr<const geometry_msgs::msg::Point>& msg) {

  if (!msg) {
    MW_ERROR("Received null message pointer!");
    return;
  }

  receive_count_++;

  MW_INFO("Received Point [{}]: x={:.2f}, y={:.2f}, z={:.2f}",
             receive_count_.load(), msg->x, msg->y, msg->z);

  // 打印Context信息
  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {}", serialization_type);
}

}  // namespace ibmw::examples::ros2_point_demo
