// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"
#include "example_event_dds_types.h"

#include <chrono>
#include <thread>

using ibmw::protocols::example::ExampleEventMsg;

namespace pubsub_demo {

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "PublisherModule Initialize");

  // Get publisher
  publisher_ = core_.GetTransport().GetPublisher("hello_topic");

  // Register DDS message type for publishing
  bool ret = ibmw::transport::RegisterSendType<ExampleEventMsg>(publisher_);
  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError, "Register publish type failed!");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "PublisherModule Initialize succeeded.");
  return true;
}

bool PublisherModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "PublisherModule Start");

  running_ = true;

  // Start publish loop in a separate thread
  publish_thread_ = std::thread([this]() {
    PublishLoop();
  });

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "PublisherModule Start succeeded.");
  return true;
}

void PublisherModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "PublisherModule Shutdown");
  running_ = false;
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "PublisherModule Shutdown succeeded.");
}

void PublisherModule::PublishLoop() {
  while (running_) {
    // Create message
    ExampleEventMsg msg;
    msg.msg("HelloWorld");
    msg.num(static_cast<int32_t>(msg_count_++));

    // Publish message
    ibmw::transport::Send(publisher_, msg);

    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "Published: msg='{}', num={}",
                 msg.msg(), msg.num());

    // Sleep for 1 second
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace pubsub_demo
