// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

#include "engine/transport/topic_descriptor.h"

namespace ibmw::runtime::core::transport {

/** Optional capability for advertising effective endpoint QoS. */
class EndpointQosProvider {
 public:
  EndpointQosProvider() = default;
  virtual ~EndpointQosProvider() = default;
  EndpointQosProvider(const EndpointQosProvider&) = delete;
  EndpointQosProvider& operator=(const EndpointQosProvider&) = delete;
  virtual QosDescriptor PublisherQos(std::string_view topic,
                                     std::string_view msg_type) const noexcept = 0;
  virtual QosDescriptor SubscriberQos(std::string_view topic,
                                      std::string_view msg_type) const noexcept = 0;
};

}  // namespace ibmw::runtime::core::transport
