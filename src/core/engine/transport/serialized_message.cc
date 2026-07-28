// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/serialized_message.h"

#include <cstring>
#include <memory>
#include <vector>

namespace ibmw::runtime::core::transport {

std::shared_ptr<ibmw::util::BufferArrayView> CopySerializedPayload(
    const ibmw::util::BufferArrayView& payload) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(payload.BufferSize());
  uint8_t* output = bytes->data();
  for (size_t index = 0; index < payload.Size(); ++index) {
    const auto& buffer = payload.Data()[index];
    if (buffer.len == 0) {
      continue;
    }
    std::memcpy(output, buffer.data, buffer.len);
    output += buffer.len;
  }

  auto* view = new ibmw::util::BufferArrayView(bytes->data(), bytes->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view,
      [bytes](const ibmw::util::BufferArrayView* owned_view) {
        delete owned_view;
      });
}

}  // namespace ibmw::runtime::core::transport
