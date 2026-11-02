// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "base/dynamiclatch.h"

namespace ibmw::util {
namespace {

// API-043: an extra CountDown is reported without corrupting the latch.
TEST(DynamicLatchTest, UnderflowKeepsCountAndClosedState) {
  DynamicLatch latch;
  ASSERT_TRUE(latch.TryAdd());
  latch.CountDown();
  EXPECT_ANY_THROW(latch.CountDown());
  EXPECT_EQ(latch.GetCurrentCount(), 0);
  EXPECT_FALSE(latch.IsClosed());
  EXPECT_TRUE(latch.TryAdd(2));
  EXPECT_ANY_THROW(latch.CountDown(3));
  EXPECT_EQ(latch.GetCurrentCount(), 2);
  latch.CountDown(2);

  latch.Close();
  EXPECT_ANY_THROW(latch.CountDown());
  EXPECT_TRUE(latch.IsClosed());
  EXPECT_EQ(latch.GetCurrentCount(), 0);
  latch.Wait();  // returns: the count is still zero
  EXPECT_FALSE(latch.TryAdd());
}

}  // namespace
}  // namespace ibmw::util
