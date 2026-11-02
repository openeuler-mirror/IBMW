// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>

#include "base/string_util.h"

namespace ibmw::common::util {
namespace {

// API-038: an empty string has no first character to capitalize.
TEST(StringUtilTest, StrToTitleCase) {
  std::string empty;
  EXPECT_EQ(StrToTitleCase(empty), "");
  std::string one = "a";
  EXPECT_EQ(StrToTitleCase(one), "A");
  std::string words = "hello big world";
  EXPECT_EQ(StrToTitleCase(words), "Hello Big World");
}

}  // namespace
}  // namespace ibmw::common::util
