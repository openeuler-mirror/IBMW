// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>
#include <iostream>

#include "internal/typesupport/dds/dds_transport.h"
#include "protocol/hello_msg.h"

namespace ibmw::examples::dds_native_demo {

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;
  transport_ = core.GetTransport();

  // Get publisher for topic
  auto publisher = transport_.GetPublisher("hello_topic");
  if (!publisher) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError, "Failed to get publisher for topic: hello_topic");
    return false;
  }

  // Register native DDS message type
  using DdsType = HelloMsg;

  bool ret = ibmw::transport::RegisterSendType<DdsType>(publisher);
  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError, "Failed to register DDS publish type");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
                "DDS Publisher module initialized. "
                "Type: ibmw::examples::dds_native_demo::HelloMsg");
  return true;
}

bool PublisherModule::Run() {
  running_ = true;
  publish_thread_ = std::thread(&PublisherModule::PublishLoop, this);
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "DDS Publisher module started.");
  return true;
}

void PublisherModule::Stop() {
  if (running_) {
    running_ = false;
    if (publish_thread_.joinable()) {
      publish_thread_.join();
    }
  }
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "DDS Publisher module shutdown.");
}

void PublisherModule::PublishLoop() {
  using DdsType = HelloMsg;

  auto publisher = transport_.GetPublisher("hello_topic");

  uint64_t count = 0;
  while (running_) {
    // Create DDS message
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count();

    DdsType msg;
    msg.id = ++count;
    msg.message = "Hello from ibmw DDS!";
    msg.timestamp = ts;

    // Publish using native DDS path
    ibmw::transport::Send<DdsType>(publisher, msg);

    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
                  "Published DDS message #{}, timestamp={}",
                  msg.id, msg.timestamp);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace ibmw::examples::dds_native_demo
