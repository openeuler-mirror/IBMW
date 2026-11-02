// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "cpp/action/action_log.h"

namespace ibmw::action {
namespace {

std::vector<std::pair<ActionLogLevel, std::string>> g_records;

void RecordingSink(ActionLogLevel level, std::string_view message) noexcept {
  g_records.emplace_back(level, std::string(message));
}

void SilentSink(ActionLogLevel, std::string_view) noexcept {}

class ActionLogTest : public testing::Test {
 protected:
  void SetUp() override { g_records.clear(); }
  void TearDown() override { SetActionLogSink(nullptr); }
};

TEST_F(ActionLogTest, DefaultWritesInfoToStdoutAndOthersToStderr) {
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  ActionLog(ActionLogLevel::kInfo, "started");
  ActionLog(ActionLogLevel::kWarn, "rejected");
  ActionLog(ActionLogLevel::kError, "failed");
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "[WARN] rejected\n[ERROR] failed\n");
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "[INFO] started\n");
}

TEST_F(ActionLogTest, SinkReceivesMessagesInsteadOfStdStreams) {
  SetActionLogSink(RecordingSink);
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  ActionLog(ActionLogLevel::kInfo, "started");
  ActionLog(ActionLogLevel::kError, "failed");
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
  ASSERT_EQ(g_records.size(), 2u);
  EXPECT_EQ(g_records[0], std::make_pair(ActionLogLevel::kInfo, std::string("started")));
  EXPECT_EQ(g_records[1], std::make_pair(ActionLogLevel::kError, std::string("failed")));
}

TEST_F(ActionLogTest, SinkCanSilenceAndNullRestoresDefault) {
  SetActionLogSink(SilentSink);
  testing::internal::CaptureStderr();
  ActionLog(ActionLogLevel::kError, "silenced");
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");

  SetActionLogSink(nullptr);
  testing::internal::CaptureStderr();
  ActionLog(ActionLogLevel::kError, "visible");
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "[ERROR] visible\n");
}

}  // namespace
}  // namespace ibmw::action
