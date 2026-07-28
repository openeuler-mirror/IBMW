// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cpp/transport/message_identity.h"
#include "cpp/transport/topic_descriptor.h"
#include "cpp/util/buffer.h"

namespace ibmw::transport {

struct SerializedMessage {
  TopicDescriptor topic;
  MessageIdentity identity;
  uint64_t publish_timestamp = 0;
  uint64_t receive_timestamp = 0;
  std::string source_driver;
  std::shared_ptr<ibmw::util::BufferArrayView> payload;

  size_t PayloadSize() const noexcept {
    return payload ? payload->BufferSize() : 0;
  }
};

using SerializedMessageCallback =
    std::function<void(SerializedMessage&& message)>;

inline std::shared_ptr<ibmw::util::BufferArrayView> CopySerializedPayload(
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
      view, [bytes](const ibmw::util::BufferArrayView* owned_view) {
        delete owned_view;
      });
}

}  // namespace ibmw::transport
