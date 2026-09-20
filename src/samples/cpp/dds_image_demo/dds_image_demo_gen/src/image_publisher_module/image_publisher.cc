// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "image_publisher_module/image_publisher.h"

#include <chrono>
#include <thread>

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, RegisterSendType<T>, publisher free functions).
#include "image_publisher_module/image_publisher_typesupport.inc.cc"

namespace dds_image_demo {

bool ImagePublisher::Setup(ibmw::Runtime core) {
  core_ = core;

  // Plugin-injected: acquire the publisher handle and bind the
  // transport-specific SenderAccess<T>. The macro is defined in
  // <name>_typesupport.inc.h so user code never needs to name a
  // transport-coupled type.
  IBMW_TS_SENDER_ACCESS_INIT(access_, core_);

  if (!(access_->RegisterSendType())) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }

  MW_INFO("ImagePublisher::Setup succeeded.");
  return true;
}

bool ImagePublisher::Run() {
  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("ImagePublisher::Run succeeded.");
  return true;
}

void ImagePublisher::Stop() {
  running_ = false;
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
  MW_INFO("ImagePublisher::Stop succeeded.");
}

void ImagePublisher::PublishLoop() {
  while (running_) {
    ibmw::examples::dds_image_demo::ImageMsg msg;
    // IBMW_PUBSUB_PUBLISHER_BODY_BEGIN
    // IBMW: user code begin (publish_body)
    // TODO: populate msg fields here
    msg_count_++;
    // IBMW: user code end (publish_body)
    // IBMW_PUBSUB_PUBLISHER_BODY_END

    // Plugin-injected publish statement (transport-neutral call
    // surface from the user's perspective).
    access_->Send(msg);

    MW_INFO("Published message #{}", msg_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace dds_image_demo
