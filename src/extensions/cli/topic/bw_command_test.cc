// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "bandwidth_statistics.h"
#include "observation_test_support.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <thread>

namespace ibmw::cli::topic {
namespace {
using namespace std::chrono_literals;
using test::Invoke;
using RegistryBwTest = test::RegistryObservationTest;

TEST(BandwidthStatisticsTest, ComputesBytesOverElapsedTimeAndEvictsOldSamples) {
  BandwidthStatistics statistics(3);
  const auto start = BandwidthStatistics::Clock::time_point{};
  statistics.Observe(100, start);
  statistics.Observe(200, start + 1s);
  statistics.Observe(300, start + 2s);
  auto result = statistics.Snapshot(start + 3s);
  ASSERT_TRUE(result.ready);
  EXPECT_DOUBLE_EQ(result.bytes_per_second, 200);
  EXPECT_DOUBLE_EQ(result.mean, 200);
  EXPECT_EQ(result.minimum, 100u);
  EXPECT_EQ(result.maximum, 300u);
  // Silence remains part of the bandwidth measurement.
  EXPECT_DOUBLE_EQ(statistics.Snapshot(start + 6s).bytes_per_second, 100);
  statistics.Observe(400, start + 4s);
  result = statistics.Snapshot(start + 5s);
  EXPECT_EQ(result.received, 4u);
  EXPECT_EQ(result.window, 3u);
  EXPECT_DOUBLE_EQ(result.bytes_per_second, 225);
  EXPECT_DOUBLE_EQ(result.mean, 300);
  EXPECT_EQ(result.minimum, 200u);
  EXPECT_EQ(result.maximum, 400u);
}

TEST(BandwidthStatisticsTest, HandlesEmptyZeroSizeAndNonIncreasingTimes) {
  EXPECT_THROW(BandwidthStatistics(0), std::invalid_argument);
  EXPECT_THROW(BandwidthStatistics(1), std::invalid_argument);
  BandwidthStatistics statistics(2);
  const auto start = BandwidthStatistics::Clock::time_point{};
  EXPECT_FALSE(statistics.Snapshot(start).ready);
  statistics.Observe(0, start + 1s);
  EXPECT_FALSE(statistics.Snapshot(start + 2s).ready);
  statistics.Observe(0, start + 1s);
  EXPECT_FALSE(statistics.Snapshot(start + 1s).ready);
  auto result = statistics.Snapshot(start + 2s);
  EXPECT_TRUE(result.ready);
  EXPECT_DOUBLE_EQ(result.bytes_per_second, 0);
  EXPECT_EQ(result.maximum, 0u);
  statistics.Observe(100, start);
  EXPECT_EQ(statistics.Snapshot(start + 1s).window, 1u);
  statistics.Observe(200, start + 1s);
  EXPECT_FALSE(statistics.Snapshot(start + 500ms).ready);
  EXPECT_DOUBLE_EQ(statistics.Snapshot(start + 2s).bytes_per_second, 150);
}

TEST(BandwidthStatisticsTest, LargePayloadSizesDoNotOverflowTheWindowTotal) {
  BandwidthStatistics statistics(2);
  const auto start = BandwidthStatistics::Clock::time_point{};
  const auto bytes = std::numeric_limits<size_t>::max();
  statistics.Observe(bytes, start);
  statistics.Observe(bytes, start + 1s);
  const auto result = statistics.Snapshot(start + 2s);
  EXPECT_DOUBLE_EQ(result.bytes_per_second, static_cast<double>(bytes));
  EXPECT_DOUBLE_EQ(result.mean, static_cast<double>(bytes));
}

TEST(BwCommandTest, ExposesHelpAndRejectsInvalidOptionsBeforeStarting) {
  const auto help = Invoke({"bw", "--help"});
  ASSERT_EQ(help.code, 0);
  for (const std::string text : {"--window", "default: 100)", "--duration", "--type", "--registry",
                                 "--dds", "--ros2", "--zenoh", "--config-file", "--qos-reliability"}) {
    EXPECT_NE(help.output.find(text), std::string::npos);
  }
  EXPECT_EQ(help.output.find("--all"), std::string::npos);
  const std::vector<std::vector<std::string>> invalid{
      {}, {"--window", "1"}, {"--window=0"}, {"--window=-1"}, {"--window=3.5"},
      {"--window=18446744073709551616"}, {"--window", "2", "--window", "3"},
      {"--duration=nan"}, {"--duration=inf"}, {"--duration=0"},
      {"--timeout=-1"}, {"--timeout=1x"}, {"--type="}, {"--dds", "--ros2"}, {"--all"},
      {"--qos-reliability", "reliable"}, {"--dds", "--qos-durability", "invalid"},
      {"--zenoh", "--qos-reliability", "reliable"}, {"--config-file", "missing"}};
  for (auto arguments : invalid) {
    SCOPED_TRACE(testing::PrintToString(arguments));
    if (!arguments.empty()) arguments.insert(arguments.begin(), "/test");
    arguments.insert(arguments.begin(), "bw");
    const auto result = Invoke(std::move(arguments));
    EXPECT_EQ(result.code, 1);
    EXPECT_TRUE(result.output.empty());
    EXPECT_FALSE(result.error.empty());
  }
}

TEST_F(RegistryBwTest, MeasuresRealPayloadBytesAndReleasesSubscription) {
  const std::string payload(1000, '\0');
  std::jthread publisher([&](std::stop_token stop) {
    while (!stop.stop_requested()) { tap.Emit(payload); std::this_thread::sleep_for(20ms); }
  });
  const auto result = Invoke({"bw", "/rate", "--registry-path", directory.string(),
                              "--duration", "0.4", "--window", "10"});
  ASSERT_EQ(result.code, 0) << result.error;
  EXPECT_NE(result.output.find("KB/s from 10 messages"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("Message size mean: 1.00 KB min: 1.00 KB max: 1.00 KB"),
            std::string::npos) << result.output;
  EXPECT_GT(std::stod(result.output), 25);
  EXPECT_LT(std::stod(result.output), 90);
  EXPECT_NE(result.error.find("Subscribed to /rate"), std::string::npos);
  EXPECT_FALSE(tap.Emit(payload));
  EXPECT_EQ(Invoke({"bw", "/rate", "--registry-path", directory.string(),
                    "--duration", "0.1"}).code, 0);
}

TEST_F(RegistryBwTest, KeepsUpdatingBandwidthDuringSilence) {
  std::jthread publisher([&](std::stop_token stop) {
    unsigned sent = 0;
    while (!stop.stop_requested() && sent < 10) {
      if (tap.Emit(std::string(1000, 'x'))) ++sent;
      std::this_thread::sleep_for(10ms);
    }
  });
  const auto result = Invoke({"bw", "/rate", "--registry-path", directory.string(), "--duration", "1.3"});
  ASSERT_EQ(result.code, 0) << result.error;
  std::istringstream output(result.output);
  std::vector<double> rates;
  for (std::string line; std::getline(output, line);) {
    if (line.find("KB/s from 10 messages") != std::string::npos) rates.push_back(std::stod(line));
  }
  ASSERT_EQ(rates.size(), 2u) << result.output;
  EXPECT_GT(rates[0], rates[1]);
  EXPECT_GT(rates[1], 0);
}

TEST_F(RegistryBwTest, ReportsNoMessagesAndMissingPublisher) {
  const auto silent = Invoke({"bw", "/rate", "--registry-path", directory.string(), "--duration", "1.05"});
  EXPECT_EQ(silent.code, 0) << silent.error;
  EXPECT_NE(silent.output.find("no new messages"), std::string::npos);
  EXPECT_NE(silent.output.find("no messages received"), std::string::npos);
  EXPECT_FALSE(tap.Emit());
  const auto missing = Invoke({"bw", "/missing", "--registry-path", directory.string(), "--timeout", "0.05"});
  EXPECT_EQ(missing.code, 1);
  EXPECT_NE(missing.error.find("Timed out waiting for a publisher"), std::string::npos);
}

}  // namespace
}  // namespace ibmw::cli::topic
