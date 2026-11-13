// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <limits>

namespace ibmw::extensions::record_playback_extension {

// Durations come from RPC requests and configuration, so timestamp arithmetic
// clamps to [0, UINT64_MAX] instead of wrapping.
inline constexpr uint64_t kMaxTimestampNs = std::numeric_limits<uint64_t>::max();

constexpr uint64_t SecondsToNsSaturated(uint64_t seconds) {
  constexpr uint64_t kNsPerSecond = 1000000000;
  return seconds > kMaxTimestampNs / kNsPerSecond ? kMaxTimestampNs : seconds * kNsPerSecond;
}

constexpr uint64_t AddSaturated(uint64_t a, uint64_t b) {
  return b > kMaxTimestampNs - a ? kMaxTimestampNs : a + b;
}

constexpr uint64_t SubSaturated(uint64_t a, uint64_t b) { return b > a ? 0 : a - b; }

}  // namespace ibmw::extensions::record_playback_extension
