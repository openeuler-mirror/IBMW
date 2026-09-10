// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/transport/serialized_message.h"

namespace ibmw::runtime::core::transport {

using ibmw::transport::MessageIdentity;
using ibmw::transport::MessageIdentityHash;
using ibmw::transport::QosDescriptor;
using ibmw::transport::SerializedMessage;
using ibmw::transport::TopicDescriptor;

/**
 * @brief Copy a potentially short-lived buffer view into owned contiguous
 * storage suitable for asynchronous recording or publishing.
 */
std::shared_ptr<ibmw::util::BufferArrayView> CopySerializedPayload(
    const ibmw::util::BufferArrayView& payload);

}  // namespace ibmw::runtime::core::transport
