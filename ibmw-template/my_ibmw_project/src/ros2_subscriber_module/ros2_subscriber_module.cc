// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_subscriber_module/ros2_subscriber_module.h"

#include "example_ros2/msg/ros_test_msg.hpp"

namespace my_project {

using example_ros2::msg::RosTestMsg;

bool Ros2SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("ros2_topic");

  // ReceiverAccess<T> wraps subscribe + type dispatch in one object.
  ibmw::transport::ReceiverAccess<RosTestMsg> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const RosTestMsg>& msg) {
        MW_INFO("Received ROS2 message.");
        // TODO: process msg fields here
      });

  if (!ret) {
    MW_ERROR("Subscribe to ROS2 type failed!");
    return false;
  }

  MW_INFO("Ros2SubscriberModule::Setup succeeded.");
  return true;
}

bool Ros2SubscriberModule::Run() {
  MW_INFO("Ros2SubscriberModule::Run succeeded.");
  return true;
}

void Ros2SubscriberModule::Stop() {
  MW_INFO("Ros2SubscriberModule::Stop succeeded.");
}

}  // namespace my_project
