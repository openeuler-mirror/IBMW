// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_publisher_module/json_publisher_module.h"

#include <chrono>
#include <thread>

namespace my_project {

bool JsonPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("json_topic");

  // Construct SenderAccess — bundles publisher + type name.
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {
    MW_ERROR("Register JSON publish type failed!");
    return false;
  }

  MW_INFO("JsonPublisherModule::Setup succeeded.");
  return true;
}

bool JsonPublisherModule::Run() {
  running_ = true;
  std::thread([this]() { PublishLoop(); }).detach();
  MW_INFO("JsonPublisherModule::Run succeeded.");
  return true;
}

void JsonPublisherModule::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("JsonPublisherModule::Stop succeeded.");
}

void JsonPublisherModule::PublishLoop() {
  while (running_) {
    SensorReading msg;
    msg.sensor_id = "sensor_01";
    msg.temperature = 20.0 + static_cast<double>(msg_count_ % 10);
    msg.humidity = 50.0 + static_cast<double>(msg_count_ % 20);
    msg.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    msg.extra_values = {1.1, 2.2, 3.3};

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published JSON: sensor={}, temp={:.1f}, humidity={:.1f}, ts={}",
            msg.sensor_id, msg.temperature, msg.humidity, msg.timestamp_ms);

    ++msg_count_;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
