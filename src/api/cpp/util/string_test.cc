// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "cpp/util/string.h"

namespace ibmw::util {
namespace {

// API-028: null pointers convert to empty strings instead of crashing.
TEST(StringUtilTest, NullPointerIsEmpty) {
  const ibmw_string_view_t from_null = ToIbmwStringView(static_cast<const char*>(nullptr));
  EXPECT_EQ(from_null.len, 0u);

  // A driver may hand back a null buffer with a non-zero length.
  const ibmw_string_view_t broken{nullptr, 5};
  EXPECT_TRUE(ToStdStringView(broken).empty());
  EXPECT_TRUE(ToStdString(broken).empty());
}

TEST(StringUtilTest, RoundTrip) {
  EXPECT_EQ(ToStdStringView(ToIbmwStringView("abc")), "abc");
  EXPECT_EQ(ToStdString(ToIbmwStringView(std::string("de"))), "de");
}

}  // namespace
}  // namespace ibmw::util
