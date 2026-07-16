// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "publisher_module.h"

namespace ibmw::examples::ros2_interop_demo {

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // 1. 获取Publisher
  publisher_ = core_.GetTransport().GetPublisher("chatter");

  // 2. 注册ROS2消息类型
  bool success = ibmw::ros2::transport::RegisterSendType<std_msgs::msg::String>(
    publisher_, "std_msgs/msg/String");

  if (!success) {
    MW_ERROR("Failed to register ROS2 type: std_msgs/msg/String");
    return false;
  }

  MW_INFO("PublisherModule initialized successfully");
  return true;
}

bool PublisherModule::Run() {
  running_ = true;

  // 启动发布线程
  publish_thread_ = std::thread([this]() { PublishLoop(); });

  MW_INFO("PublisherModule started");
  return true;
}

void PublisherModule::Stop() {
  running_ = false;

  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }

  MW_INFO("PublisherModule shutdown");
}

void PublisherModule::PublishLoop() {
  while (running_) {
    // 创建ROS2消息
    std_msgs::msg::String msg;
    msg.data = "Hello from Ibmw: " + std::to_string(publish_count_++);

    // 发布消息
    ibmw::ros2::transport::Send(
      publisher_, "std_msgs/msg/String", msg);

    MW_INFO("Published: {}", msg.data);

    // 1Hz发布频率
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace ibmw::examples::ros2_interop_demo
