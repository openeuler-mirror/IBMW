// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_publisher_module/ros2_publisher_module.h"

#include <chrono>
#include <thread>

namespace my_project {

using example_ros2::msg::RosTestMsg;

bool Ros2PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("ros2_topic");

  // Construct SenderAccess — bundles publisher + type name.
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {
    MW_ERROR("Register ROS2 publish type failed!");
    return false;
  }

  MW_INFO("Ros2PublisherModule::Setup succeeded.");
  return true;
}

bool Ros2PublisherModule::Run() {
  running_ = true;
  std::thread([this]() { PublishLoop(); }).detach();
  MW_INFO("Ros2PublisherModule::Run succeeded.");
  return true;
}

void Ros2PublisherModule::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("Ros2PublisherModule::Stop succeeded.");
}

void Ros2PublisherModule::PublishLoop() {
  while (running_) {
    auto msg = RosTestMsg();
    // TODO: populate msg fields here

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published ROS2 message #{}", msg_count_);

    ++msg_count_;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
