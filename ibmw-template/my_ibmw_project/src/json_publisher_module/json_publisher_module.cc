// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_publisher_module/json_publisher_module.h"

#include <chrono>
#include <thread>

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, RegisterSendType<T>, publisher free functions).
#include "json_publisher_module/json_publisher_module_typesupport.inc.cc"

namespace my_project {

bool JsonPublisherModule::Setup(ibmw::Runtime core) {
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

  MW_INFO("JsonPublisherModule::Setup succeeded.");
  return true;
}

bool JsonPublisherModule::Run() {
  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("JsonPublisherModule::Run succeeded.");
  return true;
}

void JsonPublisherModule::Stop() {
  running_ = false;
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
  MW_INFO("JsonPublisherModule::Stop succeeded.");
}

void JsonPublisherModule::PublishLoop() {
  while (running_) {
    SensorReading msg;
    // IBMW_PUBSUB_PUBLISHER_BODY_BEGIN
    // IBMW: user code begin (publish_body)
    msg.sensor_id = "sensor_01";
    msg.temperature = 20.0 + static_cast<double>(msg_count_ % 10);
    msg.humidity = 50.0 + static_cast<double>(msg_count_ % 20);
    msg.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    msg.extra_values = {1.1, 2.2, 3.3};
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
