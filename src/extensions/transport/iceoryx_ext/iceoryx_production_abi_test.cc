// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/iceoryx_bus.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"

#include "observability/core_metrics.h"

#include <gtest/gtest.h>

namespace ibmw::extensions::iceoryx_extension {
namespace {

TEST(IceoryxProductionAbi, ReporterIndependentClassLayouts) {
  EXPECT_EQ(sizeof(runtime::core::observability::Registry), 552U);
  EXPECT_EQ(alignof(runtime::core::observability::Registry), 8U);
  EXPECT_EQ(sizeof(IoxPublisher), 200U);
  EXPECT_EQ(alignof(IoxPublisher), 8U);
  EXPECT_EQ(sizeof(IoxSubscriptionState), 320U);
  EXPECT_EQ(alignof(IoxSubscriptionState), 8U);
  EXPECT_EQ(sizeof(IceoryxTransportDriver), 432U);
  EXPECT_EQ(alignof(IceoryxTransportDriver), 8U);
}

}  // namespace
}  // namespace ibmw::extensions::iceoryx_extension
