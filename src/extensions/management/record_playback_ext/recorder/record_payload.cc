// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/recorder/record_payload.h"

#include "base/exception.h"
#include "engine/transport/transport_driver_tools.h"

namespace ibmw::extensions::record_playback_extension {

std::shared_ptr<ibmw::util::BufferArrayView> AcquireRecordPayload(
    ibmw::runtime::core::transport::MessageFrame& message,
    std::string_view target_serialization_type,
    RecordTranscodePolicy transcode_policy) {
  MW_ASSERT(
      !target_serialization_type.empty(),
      "Target serialization type cannot be empty.");

  auto cached = message.serialization_cache.find(target_serialization_type);
  if (cached != message.serialization_cache.end()) {
    return cached->second;
  }

  if (message.msg_ptr != nullptr) {
    return ibmw::runtime::core::transport::SerializeMsgWithCache(
        message, target_serialization_type);
  }

  MW_ASSERT(
      transcode_policy == RecordTranscodePolicy::kAllow,
      "Message only contains serialized formats that do not match target '{}'; "
      "transcoding is disabled.",
      target_serialization_type);

  MW_ASSERT(
      !message.serialization_cache.empty(),
      "Message has neither a typed object nor a serialized payload.");

  return ibmw::runtime::core::transport::SerializeMsgWithCache(
      message, target_serialization_type);
}

}  // namespace ibmw::extensions::record_playback_extension
