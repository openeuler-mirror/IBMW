// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/transport/serialized_publisher.h"
#include "engine/transport/serialized_subscriber.h"

namespace ibmw::runtime::core::transport {

/**
 * @brief Optional capability interface for native serialized transport I/O.
 *
 * This is intentionally separate from TransportDriver so adding Bag support
 * does not change the existing transport plugin vtable ABI. Drivers implement
 * this interface when they gain generic serialized subscribe/publish support.
 */
class SerializedTransportDriver {
 public:
  SerializedTransportDriver() = default;
  virtual ~SerializedTransportDriver() = default;

  SerializedTransportDriver(const SerializedTransportDriver&) = delete;
  SerializedTransportDriver& operator=(const SerializedTransportDriver&) =
      delete;

  virtual SerializedSubscriberHandle AttachSerializedSubscriber(
      const SerializedSubscriberOptions& options,
      SerializedMessageCallback callback) noexcept = 0;

  virtual bool DetachSerializedSubscriber(
      SerializedSubscriberHandle handle) noexcept = 0;

  virtual SerializedPublisherHandle RegisterSerializedPublisher(
      const SerializedPublisherOptions& options) noexcept = 0;

  virtual bool SendSerialized(
      SerializedPublisherHandle handle,
      const SerializedMessage& message) noexcept = 0;
};

}  // namespace ibmw::runtime::core::transport
