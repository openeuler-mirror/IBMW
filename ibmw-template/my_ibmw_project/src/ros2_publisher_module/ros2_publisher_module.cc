// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_publisher_module/ros2_publisher_module.h"

#include <chrono>
#include <thread>

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, RegisterSendType<T>, publisher free functions).
#include "ros2_publisher_module/ros2_publisher_module_typesupport.inc.cc"

namespace my_project {

bool Ros2PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Plugin-injected: acquire the publisher handle and bind the
  // transport-specific SenderAccess<T>. The macro is defined in
  // <name>_typesupport.inc.h so user code never needs to name a
  // transport-coupled type.
  IBMW_TS_SENDER_ACCESS_INIT(access_, core_);

  if (!(access_->RegisterSendType())) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }

  MW_INFO("Ros2PublisherModule::Setup succeeded.");
  return true;
}

bool Ros2PublisherModule::Run() {
  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("Ros2PublisherModule::Run succeeded.");
  return true;
}

void Ros2PublisherModule::Stop() {
  running_ = false;
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
  MW_INFO("Ros2PublisherModule::Stop succeeded.");
}

void Ros2PublisherModule::PublishLoop() {
  while (running_) {
    example_ros2::msg::RosTestMsg msg;
    // IBMW_PUBSUB_PUBLISHER_BODY_BEGIN
    // IBMW: user code begin (publish_body)
    msg.num = static_cast<int64_t>(msg_count_);
    msg.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello" in bytes
    msg.test_msg2.num = static_cast<int32_t>(msg_count_);
    msg.test_msg2.num2 = 3.14f;
    msg.test_msg2.data = 'A';
    msg_count_++;
    // IBMW: user code end (publish_body)
    // IBMW_PUBSUB_PUBLISHER_BODY_END

    // Plugin-injected publish statement (transport-neutral call
    // surface from the user's perspective).
    access_->Send(msg);

    MW_INFO("Published message #{}", msg_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
