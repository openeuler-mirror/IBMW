// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <iostream>

#include "internal/typesupport/dds/dds_transport.h"
#include "protocol/hello_msg.h"

namespace ibmw::examples::dds_native_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;
  transport_ = core.GetTransport();

  // Get subscriber for topic
  auto subscriber = transport_.GetSubscriber("hello_topic");
  if (!subscriber) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError, "Failed to get subscriber for topic: hello_topic");
    return false;
  }

  // Subscribe to native DDS messages
  using DdsType = HelloMsg;

  auto logger = core_.GetLogger();

  bool ret = ibmw::transport::OnMessage<DdsType>(
      subscriber,
      [logger](const std::shared_ptr<const DdsType>& msg) {
        MW_LOG(logger, ibmw::common::util::kLogLevelInfo,
                      "Received DDS message: id={}, message='{}', timestamp={}",
                      msg->id, msg->message, msg->timestamp);
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError, "Failed to subscribe to DDS messages");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
                "DDS Subscriber module initialized. "
                "Type: ibmw::examples::dds_native_demo::HelloMsg");
  return true;
}

bool SubscriberModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "DDS Subscriber module started.");
  return true;
}

void SubscriberModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "DDS Subscriber module shutdown.");
}

}  // namespace ibmw::examples::dds_native_demo
