// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "json_subscriber_module/json_subscriber_module.h"

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, ReceiverAccess<T>, subscriber free functions).
#include "json_subscriber_module/json_subscriber_module_typesupport.inc.cc"

namespace my_project {

bool JsonSubscriberModule::Setup(ibmw::Runtime core) {
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
        MW_INFO("Received JSON: sensor={}, temp={:.1f}, humidity={:.1f}, ts={}",
                msg->sensor_id, msg->temperature, msg->humidity, msg->timestamp_ms);
        // IBMW: user code end (on_message_body)
        // IBMW_PUBSUB_SUBSCRIBER_BODY_END
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Subscribe failed!");
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
