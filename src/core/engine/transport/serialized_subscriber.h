// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "engine/transport/serialized_message.h"

namespace ibmw::runtime::core::transport {

struct SerializedSubscriberHandle {
  uint64_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  bool operator==(const SerializedSubscriberHandle&) const = default;
};

struct SerializedSubscriberOptionsV2 {
  TopicDescriptor topic;
  QosDescriptor requested_qos;
  // Both names are required when bypassing logical-to-wire name conversion.
  // Empty names retain the driver's normal logical topic mapping.
  std::string wire_topic_name;
  std::string wire_type_name;
};

using ibmw::transport::SerializedMessageCallback;

}  // namespace ibmw::runtime::core::transport
