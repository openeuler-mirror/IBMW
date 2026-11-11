// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <streambuf>
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

// Captures std::cout and std::cerr through their buffers: gtest's
// CaptureStdout/CaptureStderr need /tmp, which OHOS devices do not have.
class StreamCapture {
 public:
  StreamCapture()
      : old_out_(std::cout.rdbuf(out_.rdbuf())), old_err_(std::cerr.rdbuf(err_.rdbuf())) {}
  ~StreamCapture() {
    std::cout.rdbuf(old_out_);
    std::cerr.rdbuf(old_err_);
  }
  std::string Out() { return Take(out_); }
  std::string Err() { return Take(err_); }

 private:
  static std::string Take(std::ostringstream& stream) {
    std::string text = stream.str();
    stream.str({});
    return text;
  }

  std::ostringstream out_;
  std::ostringstream err_;
  std::streambuf* old_out_;
  std::streambuf* old_err_;
};

class ActionLogTest : public testing::Test {
 protected:
  void SetUp() override { g_records.clear(); }
  void TearDown() override { SetActionLogSink(nullptr); }
};

static_assert(noexcept(ActionLog(ActionLogLevel::kError, std::string_view())));

// A stream that throws on failure does not let the exception escape.
TEST_F(ActionLogTest, ThrowingStreamIsSwallowed) {
  struct FailingBuf : std::streambuf {
    int_type overflow(int_type) override { return traits_type::eof(); }
  } failing;
  auto* old_buf = std::cerr.rdbuf(&failing);
  const auto old_mask = std::cerr.exceptions();
  std::cerr.exceptions(std::ios::badbit);
  EXPECT_NO_THROW(ActionLog(ActionLogLevel::kError, "lost"));
  std::cerr.exceptions(old_mask);
  std::cerr.rdbuf(old_buf);
  std::cerr.clear();
}

TEST_F(ActionLogTest, DefaultWritesInfoToStdoutAndOthersToStderr) {
  StreamCapture capture;
  ActionLog(ActionLogLevel::kInfo, "started");
  ActionLog(ActionLogLevel::kWarn, "rejected");
  ActionLog(ActionLogLevel::kError, "failed");
  EXPECT_EQ(capture.Err(), "[WARN] rejected\n[ERROR] failed\n");
  EXPECT_EQ(capture.Out(), "[INFO] started\n");
}

TEST_F(ActionLogTest, SinkReceivesMessagesInsteadOfStdStreams) {
  SetActionLogSink(RecordingSink);
  StreamCapture capture;
  ActionLog(ActionLogLevel::kInfo, "started");
  ActionLog(ActionLogLevel::kError, "failed");
  EXPECT_EQ(capture.Err(), "");
  EXPECT_EQ(capture.Out(), "");
  ASSERT_EQ(g_records.size(), 2u);
  EXPECT_EQ(g_records[0], std::make_pair(ActionLogLevel::kInfo, std::string("started")));
  EXPECT_EQ(g_records[1], std::make_pair(ActionLogLevel::kError, std::string("failed")));
}

TEST_F(ActionLogTest, SinkCanSilenceAndNullRestoresDefault) {
  StreamCapture capture;
  SetActionLogSink(SilentSink);
  ActionLog(ActionLogLevel::kError, "silenced");
  EXPECT_EQ(capture.Err(), "");

  SetActionLogSink(nullptr);
  ActionLog(ActionLogLevel::kError, "visible");
  EXPECT_EQ(capture.Err(), "[ERROR] visible\n");
}

}  // namespace
}  // namespace ibmw::action
