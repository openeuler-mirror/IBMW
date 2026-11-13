// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

namespace ros2_interop_demo {

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Get publisher handle
  publisher_ = core_.GetTransport().GetPublisher("chatter");

  // Register ROS2 message type for publishing
  bool success = ibmw::ros2::transport::RegisterSendType<std_msgs::msg::String>(
      publisher_, "std_msgs/msg/String");

  if (!success) {
    MW_ERROR("Failed to register ROS2 type: std_msgs/msg/String");
    return false;
  }

  MW_INFO("PublisherModule::Setup succeeded.");
  return true;
}

bool PublisherModule::Run() {
  running_ = true;

  // Start publish thread (joined in Stop)
  publish_thread_ = std::thread([this]() { PublishLoop(); });

  MW_INFO("PublisherModule::Run succeeded.");
  return true;
}

void PublisherModule::Stop() {
  running_ = false;

  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }

  MW_INFO("PublisherModule::Stop succeeded.");
}

void PublisherModule::PublishLoop() {
  while (running_) {
    std_msgs::msg::String msg;
    // IBMW: user code begin (publish_body)
    msg.data = "hello #" + std::to_string(publish_count_ + 1);
    // IBMW: user code end (publish_body)
    publish_count_++;

    ibmw::ros2::transport::Send(
        publisher_, "std_msgs/msg/String", msg);

    MW_INFO("Published ROS2 interop message #{}", publish_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace ros2_interop_demo
