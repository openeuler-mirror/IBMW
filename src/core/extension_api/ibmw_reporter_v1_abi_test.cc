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

TEST(ReporterV1Abi, GoldenLayout) {
  EXPECT_EQ(sizeof(ibmw_reporter_string_view_v1), 16u);
  EXPECT_EQ(sizeof(ibmw_reporter_slot_descriptor_v1), 152u);
  EXPECT_EQ(offsetof(ibmw_reporter_slot_descriptor_v1, reserved), 88u);
}
