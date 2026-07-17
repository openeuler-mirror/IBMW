// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"
#include "example_event_dds_types.h"

#include <chrono>
#include <thread>

using ibmw::protocols::example::ExampleEventMsg;

namespace my_project {

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("example_topic");

  // Construct SenderAccess — bundles publisher + type name.
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }

  MW_INFO("PublisherModule::Setup succeeded.");
  return true;
}

bool PublisherModule::Run() {
  running_ = true;
  std::thread([this]() { PublishLoop(); }).detach();
  MW_INFO("PublisherModule::Run succeeded.");
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("PublisherModule::Stop succeeded.");
}

void PublisherModule::PublishLoop() {
  while (running_) {
    ExampleEventMsg msg;
    // TODO: populate msg fields here
    msg_count_++;

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published DDS message #{}", msg_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
