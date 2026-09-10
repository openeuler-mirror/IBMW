// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_subscriber_module/json_subscriber_module.h"

#include "cpp/util/json_transport.h"
#include "json_custom_types.h"

namespace my_project {

bool JsonSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("json_topic");

  // ReceiverAccess<T> wraps subscribe + type dispatch in one object.
  ibmw::transport::ReceiverAccess<SensorReading> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const SensorReading>& msg) {
        MW_INFO("Received JSON message.");
        // TODO: process msg fields here
      });

  if (!ret) {
    MW_ERROR("Subscribe to JSON type failed!");
    return false;
  }

  MW_INFO("JsonSubscriberModule::Setup succeeded.");
  return true;
}

bool JsonSubscriberModule::Run() {
  MW_INFO("JsonSubscriberModule::Run succeeded.");
  return true;
}

void JsonSubscriberModule::Stop() {
  MW_INFO("JsonSubscriberModule::Stop succeeded.");
}

}  // namespace my_project
