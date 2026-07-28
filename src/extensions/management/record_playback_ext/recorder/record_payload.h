// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string_view>

#include "cpp/util/buffer.h"
#include "engine/transport/message_frame.h"

namespace ibmw::extensions::record_playback_extension {

enum class RecordTranscodePolicy {
  kDisallow,
  kAllow,
};

/**
 * @brief Acquire bytes for recording while preserving an existing native
 * serialization whenever possible.
 *
 * The returned view owns or shares storage that remains valid independently
 * of the caller's stack. Existing target-format cache entries are returned
 * unchanged. Transcoding is performed only when explicitly allowed.
 */
std::shared_ptr<ibmw::util::BufferArrayView> AcquireRecordPayload(
    ibmw::runtime::core::transport::MessageFrame& message,
    std::string_view target_serialization_type,
    RecordTranscodePolicy transcode_policy = RecordTranscodePolicy::kDisallow);

}  // namespace ibmw::extensions::record_playback_extension
