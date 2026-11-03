// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/dynamic_lib.h"

#include <gtest/gtest.h>

namespace ibmw::common::util {
namespace {

// CORE-018: reading the error twice (or with none pending) must not build a
// std::string from NULL.
TEST(DynamicLib, GetErrWithoutPendingError) {
  DynamicLib lib;
  EXPECT_FALSE(lib.Load("/path/that/does/not/exist/libibmw_missing.so"));
  EXPECT_NE(DynamicLib::GetErr().find("libibmw_missing.so"), std::string::npos);
  std::string second;
  EXPECT_NO_THROW(second = DynamicLib::GetErr());
  EXPECT_FALSE(second.empty());
}

}  // namespace
}  // namespace ibmw::common::util
