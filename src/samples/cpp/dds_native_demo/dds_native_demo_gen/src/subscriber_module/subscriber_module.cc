// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, ReceiverAccess<T>, subscriber free functions).
#include "subscriber_module/subscriber_module_typesupport.inc.cc"

namespace dds_native_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Plugin-injected: produce a stack-local `access` bound to the
  // transport-specific ReceiverAccess<T> specialization. The macro is
  // defined in <name>_typesupport.inc.h so user code never needs to
  // name a transport-coupled type.
  IBMW_TS_RECEIVER_ACCESS_SETUP(core_, access);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const IBMW_TS_RECEIVER_MSG_TYPE>& msg) {
        // IBMW_PUBSUB_SUBSCRIBER_BODY_BEGIN
        // IBMW: user code begin (on_message_body)
        MW_INFO("Received message.");
        // TODO: process msg fields here
        (void)msg;
        // IBMW: user code end (on_message_body)
        // IBMW_PUBSUB_SUBSCRIBER_BODY_END
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

}  // namespace dds_native_demo
