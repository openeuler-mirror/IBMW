// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/thread_tools.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "base/exception.h"

namespace ibmw::runtime::core::util {
namespace {

// Runs on a fresh thread so a call that succeeds does not change the test
// thread; returns the exception text, or "" when nothing was thrown.
std::string ErrorOnNewThread(const std::function<void()>& call) {
  std::string error;
  std::thread([&] {
    try {
      call();
    } catch (const std::exception& e) {
      error = e.what();
    }
  }).join();
  return error;
}

// CORE-041: the priority is parsed as a whole number, not with atoi.
TEST(ThreadToolsTest, SchedPriorityMustBeANumber) {
  for (const std::string_view sched : {"SCHED_FIFO:5x", "SCHED_RR: 5", "SCHED_FIFO:+5"}) {
    const auto error = ErrorOnNewThread([sched] { SetCpuSchedForCurrentThread(sched); });
    EXPECT_NE(error.find("Invalid sched priority"), std::string::npos) << sched << ": " << error;
  }
}

// CORE-041: only the bytes of the view are read (atoi read past its end).
TEST(ThreadToolsTest, SchedPriorityStopsAtTheEndOfTheView) {
  const std::string text = "SCHED_FIFO:10123";
  const std::string_view sched(text.data(), text.size() - 3);  // "SCHED_FIFO:10"
  const auto error = ErrorOnNewThread([sched] { SetCpuSchedForCurrentThread(sched); });
  // 10 is a valid priority: it is either applied or refused by the kernel.
  EXPECT_EQ(error.find("Invalid sched priority"), std::string::npos) << error;
}

TEST(ThreadToolsTest, CpuIndexOutsideCpuSetIsRejected) {
  const auto error = ErrorOnNewThread([] { BindCpuForCurrentThread({1u << 20}); });
  EXPECT_NE(error.find("Invalid cpu index"), std::string::npos) << error;
}

}  // namespace
}  // namespace ibmw::runtime::core::util
