// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/serialized_message.h"

namespace ibmw::runtime::core::transport {

std::shared_ptr<ibmw::util::BufferArrayView> CopySerializedPayload(
    const ibmw::util::BufferArrayView& payload) {
  return ibmw::transport::CopySerializedPayload(payload);
}

}  // namespace ibmw::runtime::core::transport
