// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Unit tests for bench_options.h: yaml parsing + CSV writer.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "bench_options.h"

namespace ibmw::samples::transport_benchmark {

namespace {

// Small RAII helper for a temp file path that auto-cleans.
class TempFile {
 public:
  explicit TempFile(std::string_view content) {
    char tpl[] = "/tmp/ibmw_bench_test_XXXXXX";
    int fd = mkstemp(tpl);
    if (fd >= 0) {
      ::close(fd);
      path_ = tpl;
      std::ofstream(path_) << content;
    }
  }
  ~TempFile() {
    if (!path_.empty()) std::filesystem::remove(path_);
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

TEST(LoadBenchOptions, ParsesAllFields) {
  TempFile f(R"(
bench_options:
  payload_bytes: 4096
  hz: 200
  duration_sec: 60
  message_count: 5000
  start_delay_ms: 2000
  label: iox_send_4KB
  csv_path: /tmp/iox_send.csv
  warmup_messages: 100
)");
  ASSERT_FALSE(f.path().empty());

  auto opt = LoadBenchOptions(f.path());
  EXPECT_EQ(opt.payload_bytes, 4096u);
  EXPECT_EQ(opt.hz, 200u);
  EXPECT_EQ(opt.duration_sec, 60u);
  EXPECT_EQ(opt.message_count, 5000u);
  EXPECT_EQ(opt.start_delay_ms, 2000u);
  EXPECT_EQ(opt.label, "iox_send_4KB");
  EXPECT_EQ(opt.csv_path, "/tmp/iox_send.csv");
  EXPECT_EQ(opt.warmup_messages, 100u);
}

TEST(LoadBenchOptions, AppliesDefaultsForMissingFields) {
  TempFile f("bench_options:\n  label: minimal\n");
  ASSERT_FALSE(f.path().empty());

  auto opt = LoadBenchOptions(f.path());
  EXPECT_EQ(opt.label, "minimal");
  EXPECT_EQ(opt.payload_bytes, 1024u);   // default
  EXPECT_EQ(opt.hz, 100u);                // default
  EXPECT_EQ(opt.duration_sec, 30u);       // default
  EXPECT_EQ(opt.message_count, 0u);       // default: duration mode
  EXPECT_EQ(opt.start_delay_ms, 0u);      // default: no startup delay
  EXPECT_EQ(opt.warmup_messages, 50u);    // default
  EXPECT_TRUE(opt.csv_path.empty());      // default
}

TEST(LoadBenchOptions, ThrowsOnMissingTopLevelBlock) {
  TempFile f("other_block:\n  foo: bar\n");
  ASSERT_FALSE(f.path().empty());
  EXPECT_THROW(LoadBenchOptions(f.path()), std::runtime_error);
}

TEST(BenchCsvWriter, EmptyPathIsNoop) {
  ibmw::common::util::LatencyStats stats("noop");
  stats.Add(1000);
  EXPECT_TRUE(BenchCsvWriter::FinalizeAndAppend("", stats));
}

TEST(BenchCsvWriter, WritesHeaderOnFirstAppendThenRowsOnly) {
  // Use a fresh temp path; remove first to ensure non-existent.
  std::string csv_path;
  {
    char tpl[] = "/tmp/ibmw_bench_csv_XXXXXX";
    int fd = mkstemp(tpl);
    ASSERT_GE(fd, 0);
    ::close(fd);
    csv_path = tpl;
    // Truncate to zero-length so writer treats as needing header.
    std::ofstream(csv_path, std::ios::trunc).close();
  }

  {
    ibmw::common::util::LatencyStats s1("row_a");
    for (uint64_t i = 1; i <= 10; ++i) s1.Add(i * 1000);
    ASSERT_TRUE(BenchCsvWriter::FinalizeAndAppend(csv_path, s1));
  }
  {
    ibmw::common::util::LatencyStats s2("row_b");
    for (uint64_t i = 1; i <= 10; ++i) s2.Add(i * 2000);
    ASSERT_TRUE(BenchCsvWriter::FinalizeAndAppend(csv_path, s2));
  }

  std::ifstream in(csv_path);
  std::stringstream buf;
  buf << in.rdbuf();
  std::string content = buf.str();

  // Header on line 1; two data rows follow. Header text from LatencyStats.
  auto header = std::string(ibmw::common::util::LatencyStats::kCsvHeader);
  EXPECT_EQ(content.substr(0, header.size()), header);
  EXPECT_NE(content.find("row_a,"), std::string::npos);
  EXPECT_NE(content.find("row_b,"), std::string::npos);
  // Header must appear exactly once (no duplicate on the second append).
  size_t first = content.find("label,count");
  size_t second = content.find("label,count", first + 1);
  EXPECT_EQ(second, std::string::npos);

  std::filesystem::remove(csv_path);
}

}  // namespace ibmw::samples::transport_benchmark
