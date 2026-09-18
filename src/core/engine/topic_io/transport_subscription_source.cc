// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/transport_subscription_source.h"

#include "base/exception.h"

namespace ibmw::runtime::core::topic_io {

TransportSubscriptionSource::TransportSubscriptionSource(
    std::string driver_name,
    ibmw::runtime::core::transport::SerializedTransportOperationsV2 operations,
    ibmw::runtime::core::transport::SerializedSubscriberOptionsV2 options,
    const ibmw::common::util::LoggerWrapper& logger)
    : driver_name_(std::move(driver_name)), operations_(operations),
      options_(std::move(options)), logger_(logger) {
  MW_ASSERT(operations_.context && operations_.attach_subscriber && operations_.detach_subscriber,
            "Driver '{}' has no serialized capture operations.", driver_name_);
}

TransportSubscriptionSource::~TransportSubscriptionSource() {
  try {
    Stop();
  } catch (const std::exception& error) {
    MW_LOG(logger_, ibmw::common::util::kLogLevelError,
           "Failed to stop {} subscriber for '{}': {}.", driver_name_,
           options_.topic.topic, error.what());
  } catch (...) {
    MW_LOG(logger_, ibmw::common::util::kLogLevelError,
           "Failed to stop {} subscriber for '{}' with an unknown error.",
           driver_name_, options_.topic.topic);
  }
}

void TransportSubscriptionSource::Start(
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(!handle_, "Serialized message source is already started.");
  handle_ = operations_.attach_subscriber(operations_.context, options_, std::move(callback));
  MW_ASSERT(
      handle_, "Driver '{}' rejected serialized subscription for '{}'.",
      driver_name_, options_.topic.topic);
}

void TransportSubscriptionSource::Stop() {
  if (!handle_) {
    return;
  }
  const auto handle = handle_;
  MW_ASSERT(
      operations_.detach_subscriber(operations_.context, handle),
      "Driver '{}' failed to detach serialized subscription for '{}'.",
      driver_name_, options_.topic.topic);
  handle_ = {};
}

std::string TransportSubscriptionSource::Description() const {
  return driver_name_ + " subscriber (" + options_.topic.topic + ")";
}

}  // namespace ibmw::runtime::core::topic_io
