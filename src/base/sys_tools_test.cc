// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// API-051: the helpers have external (inline) linkage and return the exact
// executable path.

#include "base/sys_tools.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace ibmw::common::util {
namespace {

TEST(SysTools, ExecutablePathMatchesProcSelfExe) {
  const auto path = GetExecutablePath();
  EXPECT_EQ(path, std::filesystem::read_symlink("/proc/self/exe").string());
}

TEST(SysTools, PidMatchesGetpid) {
  EXPECT_EQ(GetCurrentProcessPid(), std::to_string(getpid()));
}

}  // namespace
}  // namespace ibmw::common::util
