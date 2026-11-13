// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// API-050: MW_ASSERT/MW_ASSERT_LOC use no reserved names and accept zero or
// more format arguments.

#include "base/exception.h"

#include <gtest/gtest.h>

#include <string>

namespace ibmw::common::util {
namespace {

std::string Message(void (*fn)()) {
  try {
    fn();
  } catch (const IbmwException& e) {
    return e.what();
  }
  return "<no exception>";
}

TEST(MwAssert, FormatsWithAndWithoutArguments) {
  EXPECT_NE(Message([] { MW_ASSERT(false, "plain"); }).find("]: plain"), std::string::npos);
  EXPECT_NE(Message([] { MW_ASSERT(1 + 1 == 3, "{}-{}", 7, "x"); }).find("]: 7-x"), std::string::npos);
  EXPECT_EQ(Message([] { MW_ASSERT(true, "unused {}", 1); }), "<no exception>");
}

TEST(MwAssert, LocVariantReportsGivenLocation) {
  const auto msg = Message([] {
    const auto here = SourceLocation::current();
    MW_ASSERT_LOC(here, false, "loc");
  });
  EXPECT_NE(msg.find("exception_test.cc"), std::string::npos) << msg;
  EXPECT_NE(msg.find("]: loc"), std::string::npos) << msg;
  EXPECT_NE(Message([] { MW_ASSERT_LOC(SourceLocation::current(), false, "{}", 3); }).find("]: 3"),
            std::string::npos);
}

// Names used inside the macros must not capture the caller's variables.
TEST(MwAssert, CallerVariablesAreNotShadowed) {
  const auto msg = Message([] {
    const int mw_assert_loc = 5;
    MW_ASSERT(mw_assert_loc == 4, "value {}", mw_assert_loc);
  });
  EXPECT_NE(msg.find("]: value 5"), std::string::npos) << msg;
}

}  // namespace
}  // namespace ibmw::common::util
