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
    // TODO: populate msg fields here

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published JSON message #{}", msg_count_);

    ++msg_count_;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
