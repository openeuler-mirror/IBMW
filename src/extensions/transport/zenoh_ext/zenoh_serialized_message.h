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
  if (bytes.size() < 4) throw std::invalid_argument("Truncated Zenoh envelope length.");
  ConstBufferOperator header(bytes.data(), 4);
  const auto length = header.GetUint32();
  if (length > bytes.size() - 4) throw std::invalid_argument("Truncated Zenoh envelope.");
  ConstBufferOperator body(bytes.data() + 4, length);
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
