// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CORE-015: Flush() waits for the entries already queued on the log executor,
// so the shutdown needs no fixed sleep.

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/logger/console_logger_driver.h"

namespace ibmw::runtime::core::logger {
namespace {

class ConsoleLoggerDriverTest : public testing::Test {
 protected:
  void SetUp() override {
    executor_.Initialize("log", YAML::Load("{thread_num: 1}"));
    executor_.Start();
    access_ = std::make_unique<executor::ExecutorAccess>(&executor_);
    driver_.RegisterGetExecutorFunc([this](std::string_view) {
      return ibmw::executor::ExecutorHandle(access_->NativeHandle());
    });
    saved_ = std::cout.rdbuf(captured_.rdbuf());
  }

  void TearDown() override {
    driver_.Shutdown();
    executor_.Shutdown();
    std::cout.rdbuf(saved_);
  }

  void Log(int count) {
    const std::string text = "line";
    for (int i = 0; i < count; ++i) {
      driver_.Log(LogDataWrapper{.module_name = "m",
                                 .thread_id = 0,
                                 .t = std::chrono::system_clock::now(),
                                 .lvl = ibmw_log_level_t{},
                                 .line = 1,
                                 .file_name = "f",
                                 .function_name = "fn",
                                 .log_data = text.data(),
                                 .log_data_size = text.size()});
    }
  }

  std::stringstream captured_;
  std::streambuf* saved_ = nullptr;
  executor::ThreadPoolExecutor executor_;
  std::unique_ptr<executor::ExecutorAccess> access_;
  ConsoleLoggerDriver driver_;
};

TEST_F(ConsoleLoggerDriverTest, FlushWritesEverythingLoggedBefore) {
  driver_.Initialize(YAML::Load("{color: false}"));
  Log(2000);
  driver_.Flush();
  const std::string out = captured_.str();
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 2000);
}

TEST_F(ConsoleLoggerDriverTest, FlushReturnsAtOnceWhenTheExecutorIsStopped) {
  driver_.Initialize(YAML::Load("{color: false}"));
  executor_.Shutdown();
  const auto start = std::chrono::steady_clock::now();
  driver_.Flush();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500));
}

}  // namespace
}  // namespace ibmw::runtime::core::logger
