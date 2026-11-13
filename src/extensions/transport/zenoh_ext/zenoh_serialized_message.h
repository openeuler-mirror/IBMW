// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stdexcept>
#include <string_view>

#include "base/buffer_util.h"

namespace ibmw::extensions::zenoh_extension {

struct ZenohPayloadView {
  std::string_view serialization;
  std::string_view payload;
};

inline ZenohPayloadView DecodeZenohPayload(std::string_view bytes) {
  using namespace ibmw::common::util;
  // Throws BufferOutOfBoundsException on a truncated prefix or envelope.
  const auto envelope = GetLengthPrefixedPayload(bytes.data(), bytes.size());
  ConstBufferOperator body(envelope.data(), envelope.size());
  const auto format = body.GetString(BufferLenType::kUInt8);
  if (format.empty()) throw std::invalid_argument("Missing Zenoh serialization format.");
  const auto count = body.GetUint8();
  for (size_t index = 0; index < count; ++index) {
    static_cast<void>(body.GetString(BufferLenType::kUInt16));
    static_cast<void>(body.GetString(BufferLenType::kUInt16));
  }
  const auto payload = body.GetRemainingBuffer();
  return {.serialization = format, .payload = {payload.data(), payload.size()}};
}

}  // namespace ibmw::extensions::zenoh_extension
