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

TEST(BagCommandRecordRegression, NetworkCaptureRequiresRecorderConfiguration) {
  for (const char* backend : {"--dds", "--ros2", "--zenoh"}) {
    const char* argv[] = {"ibmw", "record", "/topic", backend};
    testing::internal::CaptureStderr();
    EXPECT_EQ(IbmwRunBagCommand(4, argv), 1);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("requires --config-file"), std::string::npos);
  }
}

TEST(BagCommandRecordRegression, RejectsMixedSourcesAndInvalidDomainsBeforeRecording) {
  for (const char* value : {"-1", "4294967296", "2junk", ""}) {
    const char* argv[] = {"ibmw", "record", "/topic", "--dds", "--domain", value};
    testing::internal::CaptureStderr();
    EXPECT_EQ(IbmwRunBagCommand(6, argv), 1);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("--domain"), std::string::npos);
  }
  for (const char* backend : {"--dds", "--ros2", "--zenoh"}) {
    const char* argv[] = {"ibmw", "record", "/topic", backend, "--registry"};
    testing::internal::CaptureStderr();
    EXPECT_EQ(IbmwRunBagCommand(5, argv), 1);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("one discovery source"), std::string::npos);
  }
}

TEST(BagCommandRecordRegression, RegistryPathRequiresRegistryDiscovery) {
  for (const char* backend : {"--dds", "--ros2", "--zenoh"}) {
    const char* argv[] = {"ibmw", "record", "/topic", backend, "--registry-path", "/tmp/record-registry"};
    testing::internal::CaptureStderr();
    EXPECT_EQ(IbmwRunBagCommand(6, argv), 1);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("--registry-path requires --registry"), std::string::npos);
  }
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
