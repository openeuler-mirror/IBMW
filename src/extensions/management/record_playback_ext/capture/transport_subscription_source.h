// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/transport/serialized_transport_driver.h"
#include "record_playback_ext/capture/capture_source.h"
#include "record_playback_ext/discovery/topic_advertisement.h"

namespace ibmw::extensions::record_playback_extension {

class TransportSubscriptionSource final : public CaptureSource {
 public:
  TransportSubscriptionSource(
      std::string driver_name,
      const ibmw::runtime::core::transport::SerializedTransportCapabilityV1&
          capability,
      TopicAdvertisement advertisement,
      ibmw::transport::QosDescriptor requested_qos = {});
  ~TransportSubscriptionSource() override;

  void Start(
      ibmw::transport::SerializedMessageCallback callback)
      override;
  void Stop() override;
  std::string Description() const override;

 private:
  std::string driver_name_;
  const ibmw::runtime::core::transport::SerializedTransportCapabilityV1&
      capability_;
  TopicAdvertisement advertisement_;
  ibmw::transport::QosDescriptor requested_qos_;
  ibmw::runtime::core::transport::SerializedSubscriberHandle handle_;
};

}  // namespace ibmw::extensions::record_playback_extension
