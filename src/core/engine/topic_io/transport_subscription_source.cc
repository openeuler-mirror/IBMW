// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/transport_subscription_source.h"

#include "base/exception.h"

namespace ibmw::runtime::core::topic_io {

TransportSubscriptionSource::TransportSubscriptionSource(
    std::string driver_name,
    ibmw::runtime::core::transport::SerializedTransportDriver& driver,
    TopicAdvertisement advertisement,
    ibmw::transport::QosDescriptor requested_qos,
    const ibmw::common::util::LoggerWrapper& logger)
    : driver_name_(std::move(driver_name)),
      driver_(driver),
      advertisement_(std::move(advertisement)),
      requested_qos_(std::move(requested_qos)),
      logger_(logger) {}

TransportSubscriptionSource::~TransportSubscriptionSource() {
  try {
    Stop();
  } catch (const std::exception& error) {
    MW_LOG(logger_, ibmw::common::util::kLogLevelError,
           "Failed to stop {} subscriber for '{}': {}.", driver_name_,
           advertisement_.topic.topic, error.what());
  } catch (...) {
    MW_LOG(logger_, ibmw::common::util::kLogLevelError,
           "Failed to stop {} subscriber for '{}' with an unknown error.",
           driver_name_, advertisement_.topic.topic);
  }
}

void TransportSubscriptionSource::Start(
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(!handle_, "Serialized message source is already started.");
  const auto observed_topic = advertisement_.topic;
  handle_ = driver_.AttachSerializedSubscriber(
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
      driver_.DetachSerializedSubscriber(handle),
      "Driver '{}' failed to detach serialized subscription for '{}'.",
      driver_name_, advertisement_.topic.topic);
  handle_ = {};
}

std::string TransportSubscriptionSource::Description() const {
  return driver_name_ + " subscriber (runtime " + advertisement_.runtime_id +
         ")";
}

}  // namespace ibmw::runtime::core::topic_io
