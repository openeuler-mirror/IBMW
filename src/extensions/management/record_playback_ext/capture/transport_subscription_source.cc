// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/capture/transport_subscription_source.h"

#include <cstdio>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

TransportSubscriptionSource::TransportSubscriptionSource(
    std::string driver_name,
    const ibmw::runtime::core::transport::SerializedTransportCapabilityV1&
        capability,
    TopicAdvertisement advertisement,
    ibmw::transport::QosDescriptor requested_qos)
    : driver_name_(std::move(driver_name)),
      capability_(capability),
      advertisement_(std::move(advertisement)),
      requested_qos_(std::move(requested_qos)) {}

TransportSubscriptionSource::~TransportSubscriptionSource() {
  try {
    Stop();
  } catch (const std::exception& error) {
    std::fprintf(
        stderr, "Failed to stop %s subscriber for '%s': %s.\n",
        driver_name_.c_str(), advertisement_.topic.topic.c_str(),
        error.what());
  } catch (...) {
    std::fprintf(
        stderr,
        "Failed to stop %s subscriber for '%s' with an unknown error.\n",
        driver_name_.c_str(), advertisement_.topic.topic.c_str());
  }
}

void TransportSubscriptionSource::Start(
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(!handle_, "Capture source is already started.");
  const auto observed_topic = advertisement_.topic;
  MW_ASSERT(
      capability_.operations.attach_subscriber != nullptr,
      "Serialized capability for '{}' cannot attach subscribers.",
      driver_name_);
  handle_ = capability_.operations.attach_subscriber(
      capability_.operations.context,
      ibmw::runtime::core::transport::SerializedSubscriberOptions{
          .topic = observed_topic,
          .requested_qos = requested_qos_},
      [callback = std::move(callback), observed_topic](
          ibmw::transport::SerializedMessage&& message) {
        message.topic = observed_topic;
        callback(std::move(message));
      });
  MW_ASSERT(
      handle_, "Driver '{}' rejected serialized subscription for '{}'.",
      driver_name_, advertisement_.topic.topic);
}

void TransportSubscriptionSource::Stop() {
  if (!handle_) {
    return;
  }
  const auto handle = handle_;
  MW_ASSERT(
      capability_.operations.detach_subscriber != nullptr &&
          capability_.operations.detach_subscriber(
              capability_.operations.context, handle),
      "Driver '{}' failed to detach serialized subscription for '{}'.",
      driver_name_, advertisement_.topic.topic);
  handle_ = {};
}

std::string TransportSubscriptionSource::Description() const {
  return driver_name_ + " subscriber (runtime " + advertisement_.runtime_id +
         ")";
}

}  // namespace ibmw::extensions::record_playback_extension
