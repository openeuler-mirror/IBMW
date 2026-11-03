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

// API-039: UTF-8 bytes (negative chars) reach <cctype> as unsigned char and
// are left as they are; ASCII still folds.
TEST(StringUtilTest, NonAsciiBytes) {
  const std::string utf8 = "Gr\xc3\xbc\xc3\x9f";  // "Grüß"
  EXPECT_EQ(StrToLower(std::string_view(utf8)), "gr\xc3\xbc\xc3\x9f");
  EXPECT_EQ(StrToUpper(std::string_view(utf8)), "GR\xc3\xbc\xc3\x9f");
  EXPECT_TRUE(CheckIEqual(utf8, "GR\xc3\xbc\xc3\x9f"));
  EXPECT_FALSE(CheckIEqual("\xc3\xbc", "\xc3\x9c"));
  EXPECT_TRUE(StartsWith(utf8, "gR\xc3", true));
  EXPECT_TRUE(EndsWith(utf8, "\xc3\x9f", true));
  EXPECT_FALSE(IsAlnumStr(utf8));
  EXPECT_TRUE(IsAlnumStr("abc123"));
}

}  // namespace
}  // namespace ibmw::common::util
