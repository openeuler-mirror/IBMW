// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "record_playback_ext/time_math.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

// EXT-024/EXT-034: durations from RPC requests must not wrap timestamps.
TEST(RecordTimeMathTest, Saturates) {
  EXPECT_EQ(SecondsToNsSaturated(0), 0u);
  EXPECT_EQ(SecondsToNsSaturated(3), 3000000000u);
  EXPECT_EQ(SecondsToNsSaturated(kMaxTimestampNs / 1000000000), kMaxTimestampNs / 1000000000 * 1000000000);
  EXPECT_EQ(SecondsToNsSaturated(kMaxTimestampNs / 1000000000 + 1), kMaxTimestampNs);
  // Wraps to 5.12 ms without saturation.
  EXPECT_EQ(SecondsToNsSaturated(29549378163073488u), kMaxTimestampNs);

  const uint64_t now = 1730000000000000000u;
  EXPECT_EQ(AddSaturated(now, SecondsToNsSaturated(10)), now + 10000000000u);
  EXPECT_EQ(AddSaturated(now, SecondsToNsSaturated(UINT64_MAX)), kMaxTimestampNs);
  EXPECT_EQ(AddSaturated(now, kMaxTimestampNs - now), kMaxTimestampNs);
  EXPECT_EQ(SubSaturated(now, SecondsToNsSaturated(10)), now - 10000000000u);
  EXPECT_EQ(SubSaturated(now, SecondsToNsSaturated(1000000000000u)), 0u);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
