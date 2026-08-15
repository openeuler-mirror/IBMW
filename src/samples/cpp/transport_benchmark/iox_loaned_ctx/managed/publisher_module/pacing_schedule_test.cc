// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <chrono>

#include "gtest/gtest.h"
#include "pacing_schedule.h"

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {
namespace {
using Clock = std::chrono::steady_clock;

TEST(AdvancePacingDeadline, OnTimeAdvancesOnePeriod) {
  const auto origin = Clock::time_point{};
  const auto period = std::chrono::milliseconds(10);
  EXPECT_EQ(AdvancePacingDeadline<Clock>(origin, period, origin), origin + period);
}

TEST(AdvancePacingDeadline, OverrunAdvancesToFirstAbsoluteSlotAfterNow) {
  const auto origin = Clock::time_point{};
  const auto period = std::chrono::milliseconds(10);
  EXPECT_EQ(AdvancePacingDeadline<Clock>(origin + period, period,
                                         origin + std::chrono::milliseconds(35)),
            origin + std::chrono::milliseconds(40));
}

TEST(AdvancePacingDeadline, ExactBoundaryAdvancesPastBoundary) {
  const auto origin = Clock::time_point{};
  const auto period = std::chrono::milliseconds(10);
  EXPECT_EQ(AdvancePacingDeadline<Clock>(origin + period, period,
                                         origin + std::chrono::milliseconds(20)),
            origin + std::chrono::milliseconds(30));
}

TEST(AdvancePacingDeadline, DoesNotRebaseScheduleToNow) {
  const auto origin = Clock::time_point{};
  const auto period = std::chrono::milliseconds(10);
  const auto now = origin + std::chrono::milliseconds(35);
  const auto next = AdvancePacingDeadline<Clock>(origin, period, now);
  EXPECT_EQ(next, origin + std::chrono::milliseconds(40));
  EXPECT_NE(next, now + period);
}

TEST(BatchPacingPeriod, UnpacedIsZeroAndPacedIsNeverZero) {
  EXPECT_EQ(BatchPacingPeriod(0, 128), std::chrono::nanoseconds::zero());
  EXPECT_EQ(BatchPacingPeriod(UINT32_MAX, 1), std::chrono::nanoseconds(1));
}

TEST(AdvancePacingDeadline, ZeroPeriodDoesNotDivideByZero) {
  const auto now = Clock::time_point{} + std::chrono::milliseconds(5);
  EXPECT_EQ(AdvancePacingDeadline<Clock>(Clock::time_point{},
                                         Clock::duration::zero(), now),
            now);
}
}  // namespace
}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
