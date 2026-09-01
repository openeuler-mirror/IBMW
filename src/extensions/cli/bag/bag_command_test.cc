// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "bag_command.h"

#include <string>

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

TEST(BagCommandConfigRegression, PlaybackRequiresConfig) {
  const char* argv[] = {"ibmw", "play", "unused-bag"};
  testing::internal::CaptureStderr();
  EXPECT_EQ(IbmwRunBagCommand(3, argv), 1);
  const std::string error = testing::internal::GetCapturedStderr();
  EXPECT_NE(error.find("bag play requires --config-file"), std::string::npos);
}

TEST(BagCommandRecordRegression, RejectsSynchronousRecording) {
  const char* argv[] = {
      "ibmw", "record", "/topic", "--max-cache-size", "0"};
  testing::internal::CaptureStderr();
  EXPECT_EQ(IbmwRunBagCommand(5, argv), 1);
  const std::string error = testing::internal::GetCapturedStderr();
  EXPECT_NE(
      error.find(
          "Max cache size must be greater than zero; synchronous recording "
          "is unsupported"),
      std::string::npos);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
