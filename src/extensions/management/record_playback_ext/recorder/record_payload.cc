// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/recorder/record_payload.h"

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

std::shared_ptr<ibmw::util::BufferArrayView> AcquireRecordPayload(
    ibmw::extension::MessageView& message,
    std::string_view target_serialization_type,
    RecordTranscodePolicy transcode_policy) {
  MW_ASSERT(
      !target_serialization_type.empty(),
      "Target serialization type cannot be empty.");

  if (message.available_serialization_types.contains(
          std::string(target_serialization_type)) ||
      message.has_typed_message) {
    return message.acquire_serialized(target_serialization_type);
  }

  MW_ASSERT(
      transcode_policy == RecordTranscodePolicy::kAllow,
      "Message only contains serialized formats that do not match target '{}'; "
      "transcoding is disabled.",
      target_serialization_type);

  MW_ASSERT(
      !message.available_serialization_types.empty(),
      "Message has neither a typed object nor a serialized payload.");

  return message.acquire_serialized(target_serialization_type);
}

}  // namespace ibmw::extensions::record_playback_extension
