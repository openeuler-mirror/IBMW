// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "point_publisher_module/point_publisher_module.h"

namespace ros2_point_demo {

bool PointPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Get publisher handle
  publisher_ = core_.GetTransport().GetPublisher("point_topic");

  // Register ROS2 message type for publishing
  bool success = ibmw::ros2::transport::RegisterSendType<geometry_msgs::msg::Point>(
      publisher_, "geometry_msgs/msg/Point");

  if (!success) {
    MW_ERROR("Failed to register ROS2 type: geometry_msgs/msg/Point");
    return false;
  }

  MW_INFO("PointPublisherModule::Setup succeeded.");
  return true;
}

bool PointPublisherModule::Run() {
  running_ = true;

  // Start publish thread (joined in Stop)
  publish_thread_ = std::thread([this]() { PublishLoop(); });

  MW_INFO("PointPublisherModule::Run succeeded.");
  return true;
}

void PointPublisherModule::Stop() {
  running_ = false;

  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }

  MW_INFO("PointPublisherModule::Stop succeeded.");
}

void PointPublisherModule::PublishLoop() {
  while (running_) {
    geometry_msgs::msg::Point msg;
    // IBMW: user code begin (publish_body)
    msg.x = static_cast<double>(publish_count_ + 1);
    msg.y = msg.x * 2.0;
    msg.z = msg.x * 3.0;
    // IBMW: user code end (publish_body)
    publish_count_++;

    ibmw::ros2::transport::Send(
        publisher_, "geometry_msgs/msg/Point", msg);

    MW_INFO("Published ROS2 interop message #{}", publish_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace ros2_point_demo
