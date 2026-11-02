// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/logger/rotate_file_logger_driver.h"

namespace ibmw::runtime::core::logger {
namespace {

namespace fs = std::filesystem;

class RotateFileLoggerDriverTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("ibmw_rotate_logger_test_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    executor_.Initialize("log", YAML::Load("{thread_num: 1}"));
    executor_.Start();
    access_ = std::make_unique<executor::ExecutorAccess>(&executor_);
    driver_.RegisterGetExecutorFunc([this](std::string_view) {
      return ibmw::executor::ExecutorHandle(access_->NativeHandle());
    });
  }

  void TearDown() override {
    driver_.Shutdown();
    executor_.Shutdown();
    fs::remove_all(dir_);
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

  size_t FileCount() const {
    size_t n = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir_)) ++n;
    return n;
  }

  size_t LineCount() const {
    std::ifstream in(dir_ / "ibmw.log");
    size_t n = 0;
    for (std::string line; std::getline(in, line);) ++n;
    return n;
  }

  fs::path dir_;
  executor::ThreadPoolExecutor executor_;
  std::unique_ptr<executor::ExecutorAccess> access_;
  RotateFileLoggerDriver driver_;
};

// CORE-037: Flush() runs after the entries already queued on the log executor
// instead of touching the stream from the calling thread.
TEST_F(RotateFileLoggerDriverTest, FlushWritesEverythingLoggedBefore) {
  driver_.Initialize(YAML::Load("{path: " + dir_.string() + "}"));
  Log(2000);
  driver_.Flush();
  EXPECT_EQ(LineCount(), 2000u);
}

// CORE-037: 4096 MiB does not wrap to 0 in 32 bit, which rotated every line.
TEST_F(RotateFileLoggerDriverTest, LargeMaxFileSizeDoesNotRotateEachLine) {
  driver_.Initialize(YAML::Load("{max_file_size_m: 4096, path: " + dir_.string() + "}"));
  Log(5);
  driver_.Flush();
  EXPECT_EQ(FileCount(), 1u);
  EXPECT_EQ(LineCount(), 5u);
}

}  // namespace
}  // namespace ibmw::runtime::core::logger
