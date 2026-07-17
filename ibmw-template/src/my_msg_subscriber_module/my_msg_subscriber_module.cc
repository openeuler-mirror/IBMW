// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "my_msg_subscriber_module/my_msg_subscriber_module.h"

#include "my_msg/my_msg_dds_types.h"

using my_project::my_msg::MyMsg;

namespace my_project {

bool MyMsgSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("my_msg_topic");
  ibmw::transport::ReceiverAccess<MyMsg> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const MyMsg>& msg) {
        MW_INFO("Received: content='{}', seq={}", msg->content(), msg->seq());
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Subscribe failed!");
    return false;
  }

  MW_INFO("MyMsgSubscriberModule::Setup succeeded.");
  return true;
}

bool MyMsgSubscriberModule::Run() {
  MW_INFO("MyMsgSubscriberModule::Run succeeded.");
  return true;
}

void MyMsgSubscriberModule::Stop() {
  MW_INFO("MyMsgSubscriberModule::Stop succeeded.");
}

}  // namespace my_project
