// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace ibmw::diag {
struct CommandResult {
  int code;
  std::string output;
  std::string error;
};

// Each test executable runs sequentially; capture is restored even on a throw.
inline CommandResult Capture(int (*entry)(int, const char* const*),
                             const std::vector<std::string>& args) {
  std::vector<const char*> argv;
  for (const auto& arg : args) argv.push_back(arg.c_str());
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  try {
    const int code = entry(static_cast<int>(argv.size()), argv.data());
    auto output = testing::internal::GetCapturedStdout();
    auto error = testing::internal::GetCapturedStderr();
    return {code, std::move(output), std::move(error)};
  } catch (...) {
    (void)testing::internal::GetCapturedStdout();
    (void)testing::internal::GetCapturedStderr();
    throw;
  }
}
}  // namespace ibmw::diag
