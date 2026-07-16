// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"
#include "example_event_dds_types.h"

using ibmw::protocols::example::ExampleEventMsg;

namespace my_project {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("example_topic");

  // ReceiverAccess<T> wraps subscribe + type dispatch in one object.
  ibmw::transport::ReceiverAccess<ExampleEventMsg> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const ExampleEventMsg>& msg) {
        MW_INFO("Received: msg='{}', num={}", msg->msg(), msg->num());
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Subscribe failed!");
    return false;
  }

  MW_INFO("SubscriberModule::Setup succeeded.");
  return true;
}

bool SubscriberModule::Run() {
  MW_INFO("SubscriberModule::Run succeeded.");
  return true;
}

void SubscriberModule::Stop() {
  MW_INFO("SubscriberModule::Stop succeeded.");
}

}  // namespace my_project
