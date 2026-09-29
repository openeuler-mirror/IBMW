// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "observation_test_support.h"
#include <chrono>

namespace ibmw::cli::topic {
namespace {
using test::Invoke;

TEST(PubCommandTest, RejectsInvalidOptionsBeforeCreatingPublisher) {
  for (const auto& options : std::vector<std::vector<std::string>>{
      {"--rate", "nan"}, {"--rate", "0"}, {"--rate", "inf"},
      {"--times", "0"}, {"--times", "-1"}, {"--times", "1.5"},
      {"--once", "--times", "2"}, {"--wait", "-1"}, {"--keep-alive", "nan"},
      {"--dds"}, {"--domain", "1"}}) {
    std::vector<std::string> args{"pub", "/pub", "json:PublicationFixture", "{value: 42}", "--local"};
    args.insert(args.end(), options.begin(), options.end());
    const auto result = Invoke(args);
    EXPECT_EQ(result.code, 1) << testing::PrintToString(options);
    EXPECT_EQ(result.error.find("Publishing "), std::string::npos);
  }
}

TEST(PubCommandTest, RejectsWrongTypeAndPayloadBeforeCreatingPublisher) {
  for (const auto& payload : {"{value: bad}", "value: 1\n---\nvalue: 2"}) {
    const auto result = Invoke({"pub", "/pub", "json:PublicationFixture", payload, "--local",
        "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--once", "--wait", "0", "--keep-alive", "0"});
    EXPECT_EQ(result.code, 1);
    EXPECT_EQ(result.error.find("Publishing "), std::string::npos);
  }
  const auto result = Invoke({"pub", "/pub", "json:Unknown", "{}", "--local",
      "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--once"});
  EXPECT_EQ(result.code, 1);
  EXPECT_NE(result.error.find("No TypeSupport"), std::string::npos);
}

TEST(PubCommandTest, PublishesRequestedCountAtBoundedRateAndCanRestart) {
  const auto start = std::chrono::steady_clock::now();
  const auto repeated = Invoke({"pub", "/pub", "json:PublicationFixture", "{value: 42}", "--local",
      "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--times", "3", "--rate", "20",
      "--wait", "0", "--keep-alive", "0"});
  EXPECT_EQ(repeated.code, 0) << repeated.error;
  EXPECT_NE(repeated.error.find("Published 3 message(s)."), std::string::npos);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(90));
  const auto once = Invoke({"pub", "/pub", "json:PublicationFixture", "{value: 7}", "--local",
      "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--once", "--wait", "0", "--keep-alive", "0"});
  EXPECT_EQ(once.code, 0) << once.error;
  EXPECT_NE(once.error.find("Published 1 message(s)."), std::string::npos);
}
}  // namespace
}  // namespace ibmw::cli::topic
