// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Unified yaml options block parsed by all six transport_benchmark samples.
//
// Every sample's pub/sub yaml MUST contain a top-level `bench_options`
// node with the schema documented in BenchOptions below. The publisher and
// subscriber side may diverge on `csv_path` (publisher usually has none),
// but `payload_bytes`, `hz`, `duration_sec`, `message_count`, `label` MUST agree -- the
// subscriber uses them to size its sample reservation and label the CSV
// row, the publisher uses them to drive the send loop.

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include "yaml-cpp/yaml.h"

#include "base/benchmark_util.h"

namespace ibmw::samples::transport_benchmark {

struct BenchOptions {
  // Total user-payload bytes per message (must be >= 16 = sizeof header).
  size_t payload_bytes{1024};
  // Publish rate in Hz (publisher-side only). Subscriber ignores.
  uint32_t hz{100};
  // Run duration in seconds. Both sides shut down after this.
  uint32_t duration_sec{30};
  // Fixed-count mode. 0 keeps the historical duration-based loop; >0 makes
  // publishers stop after exactly this many messages so external harnesses can
  // compare against fixed-count competitors such as native ROS2 executors.
  uint32_t message_count{0};
  // Optional publisher-side delay after all writers are registered and the
  // engine has started. Fixed-count comparison harnesses use this as the IBMW
  // equivalent of a native ROS2 wait-for-match window.
  uint32_t start_delay_ms{0};
  // Free-form label written into the CSV `label` column. Convention:
  //   "<sample>_<size>"  e.g. "iox_loaned_pod_1KB"
  std::string label{"unlabeled"};
  // Subscriber-side: where the CSV summary row is appended after the run.
  // Empty string => no CSV output (just stdout summary).
  std::string csv_path;
  // Publisher-side warmup messages NOT included in latency stats.
  // Helps avoid first-publish JIT/COW penalties skewing percentiles.
  uint32_t warmup_messages{50};
};

// Parse the `bench_options` block from a yaml file path (typically
// returned by core.GetConfigurator().GetConfigFilePath()). Throws on
// malformed yaml; returns defaults for missing optional fields.
inline BenchOptions LoadBenchOptions(std::string_view yaml_file_path) {
  BenchOptions opt;
  YAML::Node root = YAML::LoadFile(std::string(yaml_file_path));
  YAML::Node bo = root["bench_options"];
  if (!bo) {
    throw std::runtime_error(
        std::string("LoadBenchOptions: missing top-level 'bench_options' in ") +
        std::string(yaml_file_path));
  }
  if (bo["payload_bytes"]) opt.payload_bytes = bo["payload_bytes"].as<size_t>();
  if (bo["hz"])            opt.hz = bo["hz"].as<uint32_t>();
  if (bo["duration_sec"])  opt.duration_sec = bo["duration_sec"].as<uint32_t>();
  if (bo["message_count"]) opt.message_count = bo["message_count"].as<uint32_t>();
  if (bo["start_delay_ms"]) opt.start_delay_ms = bo["start_delay_ms"].as<uint32_t>();
  if (bo["label"])         opt.label = bo["label"].as<std::string>();
  if (bo["csv_path"])      opt.csv_path = bo["csv_path"].as<std::string>();
  if (bo["warmup_messages"])
    opt.warmup_messages = bo["warmup_messages"].as<uint32_t>();
  return opt;
}

// Thread-safe CSV writer for benchmark summaries. First write to a
// previously-empty file emits the LatencyStats::kCsvHeader line; later
// writes (or writes to an existing non-empty file) only append the row.
//
// Safe to call FinalizeAndAppend() from a single thread that owns the
// LatencyStats; the mutex protects against concurrent writes from
// independent benchmark instances pointed at the same csv_path.
class BenchCsvWriter {
 public:
  // Appends one row for `stats` labelled with `stats.Label()` to
  // `csv_path`. If the file is empty / does not exist, writes
  // LatencyStats::kCsvHeader first. No-op if `csv_path` is empty.
  static bool FinalizeAndAppend(std::string_view csv_path,
                                ibmw::common::util::LatencyStats& stats) {
    if (csv_path.empty()) return true;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);

    bool need_header = false;
    {
      std::ifstream peek(std::string(csv_path), std::ios::binary | std::ios::ate);
      if (!peek.is_open() || peek.tellg() == std::streampos(0)) {
        need_header = true;
      }
    }

    std::ofstream out(std::string(csv_path),
                      std::ios::binary | std::ios::app);
    if (!out.is_open()) {
      std::cerr << "BenchCsvWriter: failed to open " << csv_path << " for append\n";
      return false;
    }
    if (need_header) {
      out << ibmw::common::util::LatencyStats::kCsvHeader;
    }
    stats.AppendCsvRow(out);
    return true;
  }
};

}  // namespace ibmw::samples::transport_benchmark
