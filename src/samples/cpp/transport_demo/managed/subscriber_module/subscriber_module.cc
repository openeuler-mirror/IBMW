// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"
#include "example_event_dds_types.h"

using ibmw::protocols::example::ExampleEventMsg;

namespace pubsub_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "SubscriberModule Initialize");

  // Get subscriber
  auto subscriber = core_.GetTransport().GetSubscriber("hello_topic");

  // Subscribe to DDS messages
  bool ret = ibmw::transport::OnMessage<ExampleEventMsg>(
      subscriber,
      [this](const std::shared_ptr<const ExampleEventMsg>& msg) {
        const std::string msg_text = msg->msg();
        const int32_t msg_num = msg->num();
        MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "Received: msg='{}', num={}",
                     msg_text, msg_num);
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError, "Subscribe failed!");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "SubscriberModule Initialize succeeded.");
  return true;
}

bool SubscriberModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "SubscriberModule Start succeeded.");
  return true;
}

void SubscriberModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "SubscriberModule Shutdown succeeded.");
}

}  // namespace pubsub_demo
