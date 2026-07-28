// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "cpp/util/buffer.h"
#include "engine/transport/message_identity.h"
#include "engine/transport/topic_descriptor.h"

namespace ibmw::runtime::core::transport {

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

/**
 * @brief Copy a potentially short-lived buffer view into owned contiguous
 * storage suitable for asynchronous recording or publishing.
 */
std::shared_ptr<ibmw::util::BufferArrayView> CopySerializedPayload(
    const ibmw::util::BufferArrayView& payload);

}  // namespace ibmw::runtime::core::transport
