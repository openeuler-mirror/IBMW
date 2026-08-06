// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "base/buffer_util.h"

namespace ibmw::extensions::iceoryx_extension::internal {

inline std::optional<uint32_t> ValidateEncodedPayloadSize(
    const void* payload, size_t payload_size) noexcept {
  if (payload == nullptr || payload_size < sizeof(uint32_t)) {
    return std::nullopt;
  }

  const auto encoded_size = common::util::GetUint32FromBuf(
      static_cast<const char*>(payload));
  if (encoded_size < sizeof(uint32_t) || encoded_size > payload_size) {
    return std::nullopt;
  }

  return encoded_size;
}

}  // namespace ibmw::extensions::iceoryx_extension::internal
