// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>

#include "engine/transport/serialized_message.h"

namespace ibmw::runtime::core::transport {

struct SerializedSubscriberHandle {
  uint64_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  bool operator==(const SerializedSubscriberHandle&) const = default;
};

struct SerializedSubscriberOptions {
  TopicDescriptor topic;
};

using ibmw::transport::SerializedMessageCallback;

}  // namespace ibmw::runtime::core::transport
