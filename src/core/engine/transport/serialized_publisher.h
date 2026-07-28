// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

#include "engine/transport/serialized_message.h"

namespace ibmw::runtime::core::transport {

struct SerializedPublisherHandle {
  uint64_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  bool operator==(const SerializedPublisherHandle&) const = default;
};

struct SerializedPublisherOptions {
  TopicDescriptor topic;
};

}  // namespace ibmw::runtime::core::transport
