// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "c/util/schema_support_base.h"

#include <cstddef>
#include <type_traits>

#include <gtest/gtest.h>

static_assert(std::is_standard_layout_v<ibmw_schema_support_v1_t>);
static_assert(std::is_trivial_v<ibmw_schema_support_v1_t>);
static_assert(offsetof(ibmw_schema_support_v1_t, abi_version) == 0);
static_assert(offsetof(ibmw_schema_support_v1_t, struct_size) == 4);
static_assert(offsetof(ibmw_schema_support_v1_t, type_name) == 8);
static_assert(offsetof(ibmw_schema_support_v1_t, reserved) == 104);
static_assert(sizeof(ibmw_schema_support_v1_t) == 168);
static_assert(IBMW_SCHEMA_SUPPORT_V1_REQUIRED_SIZE ==
              offsetof(ibmw_schema_support_v1_t, reserved));

static_assert(std::is_standard_layout_v<ibmw_schema_support_package_v1_t>);
static_assert(std::is_trivial_v<ibmw_schema_support_package_v1_t>);
static_assert(offsetof(ibmw_schema_support_package_v1_t, abi_version) == 0);
static_assert(offsetof(ibmw_schema_support_package_v1_t, struct_size) == 4);
static_assert(offsetof(ibmw_schema_support_package_v1_t, entry_count) == 8);
static_assert(IBMW_SCHEMA_SUPPORT_PACKAGE_V1_REQUIRED_SIZE ==
              offsetof(ibmw_schema_support_package_v1_t, reserved));

TEST(SchemaSupportV1Abi, GoldenLayout) {
  EXPECT_EQ(sizeof(ibmw_string_view_t), 16U);
  EXPECT_EQ(offsetof(ibmw_schema_support_package_v1_t, entries), 16U);
  EXPECT_EQ(offsetof(ibmw_schema_support_package_v1_t, reserved), 24U);
  EXPECT_EQ(sizeof(ibmw_schema_support_package_v1_t), 88U);
  EXPECT_EQ(IBMW_SCHEMA_SUPPORT_V1_PREFIX_SIZE, 8U);
}
