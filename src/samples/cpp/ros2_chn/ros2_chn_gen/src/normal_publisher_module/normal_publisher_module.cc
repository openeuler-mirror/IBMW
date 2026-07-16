// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_publisher_module/normal_publisher_module.h"

#include <chrono>
#include <thread>

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, RegisterSendType<T>, publisher free functions).
#include "normal_publisher_module/normal_publisher_module_typesupport.inc.cc"

namespace ros2_chn_demo {

bool NormalPublisherModule::Setup(ibmw::Runtime core) {
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

  MW_INFO("NormalPublisherModule::Setup succeeded.");
  return true;
}

bool NormalPublisherModule::Run() {
  running_ = true;
  std::thread([this]() { PublishLoop(); }).detach();
  MW_INFO("NormalPublisherModule::Run succeeded.");
  return true;
}

void NormalPublisherModule::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("NormalPublisherModule::Stop succeeded.");
}

void NormalPublisherModule::PublishLoop() {
  while (running_) {
    example_ros2::msg::RosTestMsg msg;
    // IBMW_PUBSUB_PUBLISHER_BODY_BEGIN
    // IBMW: user code begin (publish_body)
    // TODO: populate msg fields here
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

}  // namespace ros2_chn_demo
