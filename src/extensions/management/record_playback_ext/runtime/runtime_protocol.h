// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/runtime_graph/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {
using ibmw::runtime::core::runtime_graph::ConfigureRuntimeSocket;
using ibmw::runtime::core::runtime_graph::RequireSameUid;
using ibmw::runtime::core::runtime_graph::RuntimeStreamHeader;
using ibmw::runtime::core::runtime_graph::kRuntimeMaxControlMessage;
using ibmw::runtime::core::runtime_graph::kRuntimeMaxStreamHeader;
using ibmw::runtime::core::runtime_graph::kRuntimeMaxStreamPayload;
using ibmw::runtime::core::runtime_graph::kRuntimeProtocolVersion;
using ibmw::runtime::core::runtime_graph::kRuntimeSocketTimeout;
using ibmw::runtime::core::runtime_graph::kRuntimeTapMagic;

inline constexpr uint32_t kRuntimePlaybackMagic = 0x4942504c;
inline constexpr uint32_t kRuntimePlaybackTerminalMagic = 0x49425054;

struct RuntimeTerminalHeader {
  uint32_t magic;
  uint32_t protocol_version;
  uint32_t json_size;
  uint32_t reserved;
};
static_assert(sizeof(RuntimeTerminalHeader) == 16);
}  // namespace ibmw::extensions::record_playback_extension
