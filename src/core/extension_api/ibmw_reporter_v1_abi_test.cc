// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_reporter_v1.h"

#include <cstddef>
#include <type_traits>

#include <gtest/gtest.h>

static_assert(std::is_standard_layout_v<ibmw_reporter_v1>);
static_assert(std::is_trivial_v<ibmw_reporter_v1>);
static_assert(offsetof(ibmw_reporter_v1, abi_version) == 0);
static_assert(offsetof(ibmw_reporter_v1, struct_size) == 4);
static_assert(offsetof(ibmw_reporter_v1, owner) == 8);
static_assert(offsetof(ibmw_reporter_v1, register_slot) == 16);
static_assert(offsetof(ibmw_reporter_v1, add) == 24);
static_assert(offsetof(ibmw_reporter_v1, reserved) == 32);
static_assert(offsetof(ibmw_reporter_v1, add_signed) == 96);
static_assert(sizeof(ibmw_reporter_v1) == 104);
static_assert(std::is_standard_layout_v<ibmw_reporter_batch_v1>);
static_assert(std::is_trivial_v<ibmw_reporter_batch_v1>);
static_assert(offsetof(ibmw_reporter_batch_v1, abi_version) == 0);
static_assert(offsetof(ibmw_reporter_batch_v1, struct_size) == 4);
static_assert(offsetof(ibmw_reporter_batch_v1, owner) == 8);
static_assert(offsetof(ibmw_reporter_batch_v1, prepare) == 16);
static_assert(offsetof(ibmw_reporter_batch_v1, commit) == 24);
static_assert(offsetof(ibmw_reporter_batch_v1, abort) == 32);
static_assert(sizeof(ibmw_reporter_batch_v1) == 40);

TEST(ReporterV1Abi, GoldenLayout) {
  EXPECT_EQ(sizeof(ibmw_reporter_string_view_v1), 16u);
  EXPECT_EQ(sizeof(ibmw_reporter_slot_descriptor_v1), 152u);
  EXPECT_EQ(offsetof(ibmw_reporter_slot_descriptor_v1, reserved), 88u);
}

TEST(ReporterV1Abi, BatchCompanionHasVersionedBoundary) {
  EXPECT_EQ(offsetof(ibmw_reporter_batch_v1, owner), 8u);
  EXPECT_EQ(IBMW_REPORTER_BATCH_ABI_VERSION_V1, 1u);
  EXPECT_EQ(sizeof(ibmw_reporter_batch_v1), 40u);
}
