// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Included first: the header must compile on its own (API-052).
#include "base/light_signal.h"

#include <gtest/gtest.h>

#include <thread>

namespace ibmw::common::util {
namespace {

using namespace std::chrono_literals;

TEST(LightSignalTest, NotifyReleasesWaitersUntilReset) {
  LightSignal signal;
  EXPECT_FALSE(signal.WaitFor(1ms));

  std::thread waiter([&signal] { signal.Wait(); });
  signal.Notify();
  waiter.join();
  EXPECT_TRUE(signal.WaitFor(0ms));  // stays set for later waiters

  signal.Reset();
  EXPECT_FALSE(signal.WaitFor(1ms));
}

}  // namespace
}  // namespace ibmw::common::util
