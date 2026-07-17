// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "my_msg_publisher_module/my_msg_publisher_module.h"

#include <chrono>
#include <thread>

namespace my_project {

bool MyMsgPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("my_msg_topic");
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }

  MW_INFO("MyMsgPublisherModule::Setup succeeded.");
  return true;
}

bool MyMsgPublisherModule::Run() {
  running_ = true;
  std::thread([this]() { PublishLoop(); }).detach();
  MW_INFO("MyMsgPublisherModule::Run succeeded.");
  return true;
}

void MyMsgPublisherModule::Stop() {
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("MyMsgPublisherModule::Stop succeeded.");
}

void MyMsgPublisherModule::PublishLoop() {
  while (running_) {
    my_project::my_msg::MyMsg msg;
    msg.content("Hello from Demo 8 (bring-your-own IDL)!");
    msg.seq(seq_++);
    access_->Send(msg);
    MW_INFO("Published: content='{}', seq={}", msg.content(), msg.seq());
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace my_project
