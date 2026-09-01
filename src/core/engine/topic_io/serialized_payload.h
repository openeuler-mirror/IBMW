// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string_view>

#include "extension_api/ibmw_extension_runtime.h"

namespace ibmw::runtime::core::topic_io {

enum class SerializedPayloadPolicy {
  kDisallow,
  kAllow,
};

/**
 * @brief Acquire bytes while preserving an existing native
 * serialization whenever possible.
 *
 * The returned view owns or shares storage that remains valid independently
 * of the caller's stack. Existing target-format cache entries are returned
 * unchanged. Transcoding is performed only when explicitly allowed.
 */
std::shared_ptr<ibmw::util::BufferArrayView> AcquireSerializedPayload(
    ibmw::extension::MessageView& message,
    std::string_view target_serialization_type,
    SerializedPayloadPolicy transcode_policy = SerializedPayloadPolicy::kDisallow);

}  // namespace ibmw::runtime::core::topic_io
