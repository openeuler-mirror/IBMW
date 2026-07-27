// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <limits>

#include "iceoryx_ext/internal/checked_size.h"
#include "iceoryx_ext/iceoryx_bus.h"

namespace ibmw::extensions::iceoryx_extension::internal {
namespace {

TEST(CheckedSize, AddsWithoutOverflow) {
  size_t result = 0;
  ASSERT_TRUE(CheckedAddSize(9, 17, result));
  EXPECT_EQ(result, 26U);
}

TEST(CheckedSize, RejectsOverflowWithoutExposingWrappedResult) {
  size_t result = 123;
  EXPECT_FALSE(CheckedAddSize(std::numeric_limits<size_t>::max(), 1, result));
  EXPECT_EQ(result, 123U);

  EXPECT_FALSE(CheckedAddSize(9, std::numeric_limits<size_t>::max(), result));
  EXPECT_EQ(result, 123U);
}

TEST(IoxShmSize, NormalizesZeroToSafeMinimum) {
  ASSERT_EQ(NextIoxShmSize(0, 0), 1U);
}

TEST(IoxShmSize, DoublesUntilMinimumIsCovered) {
  ASSERT_EQ(NextIoxShmSize(8, 17), 32U);
  ASSERT_EQ(NextIoxShmSize(32, 17), 32U);
}

TEST(IoxShmSize, RejectsPayloadsBeyondIceoryxLimitWithoutLooping) {
  if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
    EXPECT_FALSE(NextIoxShmSize(
        1, static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U));
  }
  EXPECT_FALSE(NextIoxShmSize(std::numeric_limits<size_t>::max(), 1));
}

}  // namespace
}  // namespace ibmw::extensions::iceoryx_extension::internal
