// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Unit tests for src/base/benchmark_util.h
//   - MonotonicNs()  : sanity (monotonic non-decreasing, sub-ms resolution)
//   - LatencyStats   : add/percentile/avg/stddev/CSV row format

#include <gtest/gtest.h>

#include <sstream>
#include <thread>

#include "base/benchmark_util.h"

namespace ibmw::common::util {

TEST(MonotonicNs, IsNonDecreasing) {
  uint64_t prev = MonotonicNs();
  for (int i = 0; i < 1000; ++i) {
    uint64_t now = MonotonicNs();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(MonotonicNs, AdvancesOverShortSleep) {
  uint64_t a = MonotonicNs();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  uint64_t b = MonotonicNs();
  // At least 1 ms elapsed (allow scheduling slack), at most 1 s.
  ASSERT_GT(b, a);
  ASSERT_GT(b - a, 1'000'000ULL);
  ASSERT_LT(b - a, 1'000'000'000ULL);
}

TEST(LatencyStats, EmptyComputeReturnsZeros) {
  LatencyStats s("empty");
  auto sum = s.Compute();
  EXPECT_EQ(sum.count, 0u);
  EXPECT_DOUBLE_EQ(sum.avg_ns, 0.0);
  EXPECT_DOUBLE_EQ(sum.p99_ns, 0.0);
}

TEST(LatencyStats, BasicSequence1To100) {
  LatencyStats s("seq");
  for (uint64_t i = 1; i <= 100; ++i) s.Add(i);
  auto sum = s.Compute();
  EXPECT_EQ(sum.count, 100u);
  EXPECT_DOUBLE_EQ(sum.min_ns, 1.0);
  EXPECT_DOUBLE_EQ(sum.max_ns, 100.0);
  EXPECT_DOUBLE_EQ(sum.avg_ns, 50.5);
  // Nearest-rank percentile: ceil(0.50 * 100) = 50 -> sample 50.
  EXPECT_DOUBLE_EQ(sum.p50_ns, 50.0);
  EXPECT_DOUBLE_EQ(sum.p90_ns, 90.0);
  EXPECT_DOUBLE_EQ(sum.p99_ns, 99.0);
  // ceil(0.999 * 100) = 100 -> sample 100.
  EXPECT_DOUBLE_EQ(sum.p999_ns, 100.0);
  EXPECT_GT(sum.stddev_ns, 0.0);
}

TEST(LatencyStats, CsvHeaderAndRow) {
  LatencyStats s("dds_send");
  for (uint64_t i = 0; i < 10; ++i) s.Add(1000ULL * (i + 1));  // 1us..10us
  std::ostringstream out;
  out << LatencyStats::kCsvHeader;
  s.AppendCsvRow(out);
  std::string csv = out.str();
  // Header line + 1 data line, each ending with newline.
  EXPECT_NE(csv.find("label,count"), std::string::npos);
  EXPECT_NE(csv.find("dds_send,10,"), std::string::npos);
  // Min should be 1.0 us, max 10.0 us in the row.
  EXPECT_NE(csv.find(",1,"), std::string::npos);   // min_us=1
  EXPECT_NE(csv.find(",10\n"), std::string::npos); // max_us=10 at end
}

TEST(LatencyStats, HumanStringContainsLabelAndStats) {
  LatencyStats s("iceoryx_loaned");
  for (uint64_t i = 0; i < 50; ++i) s.Add(2000);  // constant 2us
  std::string h = s.ToHumanString();
  EXPECT_NE(h.find("iceoryx_loaned"), std::string::npos);
  EXPECT_NE(h.find("n=50"), std::string::npos);
  EXPECT_NE(h.find("avg=2.00us"), std::string::npos);
  EXPECT_NE(h.find("p99=2.00us"), std::string::npos);
}

}  // namespace ibmw::common::util
