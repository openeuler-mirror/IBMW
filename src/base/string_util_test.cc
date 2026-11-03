// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <utility>

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

// API-040: ReplaceEnvVars without std::regex keeps the old rules.
TEST(StringUtilTest, ReplaceEnvVarsKeepsRules) {
  ::setenv("IBMW_A040_X", "val", 1);
  ::setenv("IBMW_A040_NEST", "${IBMW_A040_X}", 1);
  ::unsetenv("IBMW_A040_NONE");
  const std::pair<std::string, std::string> cases[] = {
      {"", ""},
      {"plain", "plain"},
      {"${IBMW_A040_X}", "val"},
      {"a${IBMW_A040_X}b${IBMW_A040_X}c", "avalbvalc"},
      {"${IBMW_A040_NONE}|", "|"},
      {"${}", "${}"},
      {"${}${IBMW_A040_X}", "${}val"},
      {"${IBMW_A040_X", "${IBMW_A040_X"},
      {"$IBMW_A040_X", "$IBMW_A040_X"},
      {"${IBMW_A040_NEST}", "${IBMW_A040_X}"},  // values are not expanded again
      {"${a${IBMW_A040_X}}", "}"},
      {"}${IBMW_A040_X}{", "}val{"},
      {"$${IBMW_A040_X}", "$val"},
      {"${${IBMW_A040_X}", ""},
  };
  for (const auto& [in, out] : cases) EXPECT_EQ(ReplaceEnvVars(in), out) << in;
}

TEST(StringUtilTest, ReplaceStringAllLengthCases) {
  const std::array<std::array<std::string, 4>, 8> cases{{
      {"aXbXc", "X", "Y", "aYbYc"},
      {"aXXbXXc", "XX", "Y", "aYbYc"},
      {"XXaXX", "XX", "", "a"},
      {"aXb", "X", "YYY", "aYYYb"},
      {"XXX", "X", "ZZ", "ZZZZZZ"},
      {"abc", "q", "r", "abc"},
      {"aaaa", "aa", "b", "bb"},
      {"", "a", "b", ""},
  }};
  for (const auto& c : cases) {
    std::string s = c[0];
    EXPECT_EQ(ReplaceString(s, c[1], c[2]), c[3]) << c[0] << " " << c[1] << " " << c[2];
  }
}

}  // namespace
}  // namespace ibmw::common::util
