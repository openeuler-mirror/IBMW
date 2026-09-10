// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <string_view>

#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "record_playback_ext/record_action.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

class RecordActionExecutorFixture : public testing::Test {
 protected:
  void SetUp() override {
    executor_.Initialize(
        "record-action-shutdown-test", YAML::Load("{thread_num: 1}"));
    executor_.Start();
    access_ = std::make_unique<runtime::core::executor::ExecutorAccess>(
        &executor_);
  }

  void TearDown() override { executor_.Shutdown(); }

  ibmw::executor::ExecutorHandle Handle() const {
    return ibmw::executor::ExecutorHandle(access_->NativeHandle());
  }

  void InitializeAction(RecordAction& action) {
    action.RegisterGetExecutorFunc([this](std::string_view name) {
      return name == "record-action-shutdown-test"
                 ? Handle()
                 : ibmw::executor::ExecutorHandle();
    });
    action.RegisterGetTypeSupportFunc(
        [](std::string_view) { return ibmw::util::TypeSupportRef(); });
    action.RegisterGetSchemaSupportFunc(
        [](std::string_view, std::string_view)
            -> std::optional<ibmw::util::SchemaDescriptor> {
          return std::nullopt;
        });
    action.Initialize(YAML::Load(
        "{bag_path: /tmp, mode: signal, executor: "
        "record-action-shutdown-test, topic_meta_list: []}"));
    action.InitExecutor(Handle());
  }

  runtime::core::executor::ThreadPoolExecutor executor_;
  std::unique_ptr<runtime::core::executor::ExecutorAccess> access_;
};

TEST_F(RecordActionExecutorFixture, ShutdownCompletesInsideRecordExecutor) {
  RecordAction action;
  InitializeAction(action);
  action.Start();
  std::promise<void> finished;
  auto finished_future = finished.get_future();
  Handle().Dispatch([&] {
    action.Shutdown();
    finished.set_value();
  });

  ASSERT_EQ(
      finished_future.wait_for(std::chrono::seconds(1)),
      std::future_status::ready);
}

TEST_F(RecordActionExecutorFixture, ShutdownCompletesFromAnotherThread) {
  RecordAction action;
  InitializeAction(action);
  action.Start();
  auto shutdown =
      std::async(std::launch::async, [&] { action.Shutdown(); });

  ASSERT_EQ(
      shutdown.wait_for(std::chrono::seconds(1)),
      std::future_status::ready);
  EXPECT_NO_THROW(shutdown.get());
}

TEST_F(RecordActionExecutorFixture, RejectedShutdownTaskHasBoundedFailure) {
  RecordAction action;
  InitializeAction(action);
  executor_.Shutdown();
  const auto started = std::chrono::steady_clock::now();
  EXPECT_NO_THROW(action.Shutdown());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(1));
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
