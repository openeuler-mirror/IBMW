// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "point_publisher_module.h"

namespace ibmw::examples::ros2_point_demo {

bool PointPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // 1. 获取Publisher
  publisher_ = core_.GetTransport().GetPublisher("point_topic");

  // 2. 注册ROS2消息类型 (geometry_msgs::msg::Point - Plain类型)
  bool success = ibmw::ros2::transport::RegisterSendType<geometry_msgs::msg::Point>(
    publisher_, "geometry_msgs/msg/Point");

  if (!success) {
    MW_ERROR("Failed to register ROS2 type: geometry_msgs/msg/Point");
    return false;
  }

  MW_INFO("PointPublisherModule initialized successfully");
  MW_INFO("Publishing Plain type: geometry_msgs::msg::Point");
  return true;
}

bool PointPublisherModule::Run() {
  running_ = true;

  // 启动发布线程
  publish_thread_ = std::thread([this]() { PublishLoop(); });

  MW_INFO("PointPublisherModule started");
  return true;
}

void PointPublisherModule::Stop() {
  running_ = false;

  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }

  MW_INFO("PointPublisherModule shutdown");
}

void PointPublisherModule::PublishLoop() {
  while (running_) {
    // 创建ROS2 Point消息 (Plain类型)
    geometry_msgs::msg::Point msg;
    msg.x = 1.0 + publish_count_ * 0.1;
    msg.y = 2.0 + publish_count_ * 0.2;
    msg.z = 3.0 + publish_count_ * 0.3;

    // 发布消息
    ibmw::ros2::transport::Send(
      publisher_, "geometry_msgs/msg/Point", msg);

    MW_INFO("Published Point [{}]: x={:.2f}, y={:.2f}, z={:.2f}",
               publish_count_, msg.x, msg.y, msg.z);

    publish_count_++;

    // 1Hz发布频率
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace ibmw::examples::ros2_point_demo
