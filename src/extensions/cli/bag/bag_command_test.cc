// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "bag_command.h"

#include <string>
#include <utility>

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

// EXT-046: numeric options reject partial, signed, out-of-range and
// non-finite values instead of truncating them or throwing out of the parser.
TEST(BagCommandRecordRegression, RejectsInvalidNumericOptions) {
  const std::pair<const char*, const char*> cases[] = {
      {"--max-cache-size", "10abc"},        {"--max-cache-size", "-1"},
      {"--polling-interval", "4294967296"}, {"--polling-interval", "1.5"},
      {"--max-bag-size", "99999999999999999999"}, {"--max-bag-duration", "nan"},
      {"--max-bag-duration", "3s"},         {"--compression-threads", " 2"}};
  for (const auto& [option, value] : cases) {
    const char* argv[] = {"ibmw", "record", "/topic", option, value};
    testing::internal::CaptureStderr();
    EXPECT_EQ(IbmwRunBagCommand(5, argv), 1) << option << " " << value;
    EXPECT_NE(testing::internal::GetCapturedStderr().find(option), std::string::npos)
        << option << " " << value;
  }
  const std::string joined = "--max-cache-size=12x";
  const char* argv[] = {"ibmw", "record", "/topic", joined.c_str()};
  testing::internal::CaptureStderr();
  EXPECT_EQ(IbmwRunBagCommand(4, argv), 1);
  EXPECT_NE(testing::internal::GetCapturedStderr().find(joined), std::string::npos);
}

TEST(BagCommandPlayRegression, RejectsInvalidRate) {
  for (const char* value : {"inf", "2x", ""}) {
    const char* argv[] = {"ibmw", "play", "unused-bag", "--rate", value};
    testing::internal::CaptureStderr();
    EXPECT_EQ(IbmwRunBagCommand(5, argv), 1) << value;
    EXPECT_NE(testing::internal::GetCapturedStderr().find("--rate"), std::string::npos) << value;
  }
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
