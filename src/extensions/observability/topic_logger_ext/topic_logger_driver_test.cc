// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "topic_logger_ext/topic_logger_driver.h"

namespace ibmw::extensions::topic_logger_extension {
namespace {

// EXT-048: the publisher hooks run even if Initialize() never created the timer.
TEST(TopicLoggerDriverTest, PublisherHooksBeforeInitializeAreSafe) {
  TopicLoggerDriver driver;
  driver.StartupPublisher();
  driver.StopPublisher();
}

// EXT-048: module names are string_views and are printed with their length.
TEST(TopicLoggerDriverTest, RegexErrorPrintsOnlyTheModuleName) {
  runtime::core::executor::ThreadPoolExecutor executor;
  executor.Initialize("timer", YAML::Load("{thread_num: 1}"));
  executor.Start();
  runtime::core::executor::ExecutorAccess access(&executor);

  TopicLoggerDriver driver;
  driver.RegisterGetExecutorFunc([&access](std::string_view) {
    return ibmw::executor::ExecutorHandle(access.NativeHandle());
  });
  driver.Initialize(YAML::Load("{topic: /log, timer_executor_name: timer, module_filter: '('}"));

  const std::string storage = "modTRAILING";
  runtime::core::logger::LogDataWrapper log{};
  log.module_name = std::string_view(storage).substr(0, 3);
  log.file_name = "f";
  log.function_name = "g";
  log.log_data = "";
  testing::internal::CaptureStderr();
  driver.Log(log);
  const std::string error = testing::internal::GetCapturedStderr();
  EXPECT_NE(error.find("string: mod,"), std::string::npos) << error;
  EXPECT_EQ(error.find("TRAILING"), std::string::npos) << error;

  driver.StopPublisher();
  executor.Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::topic_logger_extension
