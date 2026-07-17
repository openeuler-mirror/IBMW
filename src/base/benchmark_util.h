// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Cross-process latency measurement utilities for IBMW benchmarks.
//
// Why a dedicated header (instead of reusing time_util.h)?
//   - time_util.h templates default to std::chrono::system_clock, which is
//     wall-clock and may jump backwards on NTP correction.
//   - For latency micro-benchmarks across processes we want a monotonic clock
//     that is *coherent across processes on the same host*. On Linux, both
//     CLOCK_MONOTONIC and the libstdc++ std::chrono::steady_clock satisfy that.
//   - We expose the raw clock_gettime(CLOCK_MONOTONIC) wrapper so the timestamp
//     embedded in a payload by process A can be subtracted from process B's
//     read on the exact same clock source. CLOCK_MONOTONIC_RAW is intentionally
//     NOT used (no NTP slewing => coherent across CPUs without further work).
//
// Provides:
//   ibmw::common::util::MonotonicNs()       - uint64_t ns since arbitrary epoch
//   ibmw::common::util::LatencyStats        - online sample collector with
//                                             min/avg/p50/p90/p99/p99.9/max
//                                             + jitter (stddev) + CSV export.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace ibmw::common::util {

// Returns CLOCK_MONOTONIC nanoseconds. Suitable for cross-process latency
// measurement on the same host.
inline uint64_t MonotonicNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Online latency-sample collector for benchmarks.
// Stores all samples in-memory (typical bench: 30s * 100Hz = 3000 samples,
// 24KB). Use Reserve() if a larger run is planned.
class LatencyStats {
 public:
  struct Summary {
    size_t count{0};
    double min_ns{0};
    double max_ns{0};
    double avg_ns{0};
    double stddev_ns{0};   // jitter
    double p50_ns{0};
    double p90_ns{0};
    double p99_ns{0};
    double p999_ns{0};
  };

  LatencyStats() = default;
  explicit LatencyStats(std::string label) : label_(std::move(label)) {}

  void Reserve(size_t n) { samples_.reserve(n); }
  void Add(uint64_t latency_ns) { samples_.push_back(latency_ns); }
  size_t Count() const { return samples_.size(); }
  const std::string& Label() const { return label_; }
  const std::vector<uint64_t>& Samples() const { return samples_; }

  // Computes summary. Sorts samples internally (mutable view via Compute()
  // after which Samples() is sorted ascending).
  Summary Compute() {
    Summary s;
    s.count = samples_.size();
    if (s.count == 0) return s;

    std::sort(samples_.begin(), samples_.end());
    s.min_ns = static_cast<double>(samples_.front());
    s.max_ns = static_cast<double>(samples_.back());

    long double sum = 0.0L;
    for (auto v : samples_) sum += static_cast<long double>(v);
    s.avg_ns = static_cast<double>(sum / static_cast<long double>(s.count));

    long double var = 0.0L;
    for (auto v : samples_) {
      long double d = static_cast<long double>(v) - s.avg_ns;
      var += d * d;
    }
    s.stddev_ns = static_cast<double>(std::sqrt(var / static_cast<long double>(s.count)));

    s.p50_ns = Percentile(0.50);
    s.p90_ns = Percentile(0.90);
    s.p99_ns = Percentile(0.99);
    s.p999_ns = Percentile(0.999);
    return s;
  }

  // CSV header for AppendCsvRow(). Caller writes once per file.
  static constexpr std::string_view kCsvHeader =
      "label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\n";

  // Appends one CSV row to `out` using us (microsecond) units.
  void AppendCsvRow(std::ostream& out) {
    auto s = Compute();
    auto to_us = [](double ns) { return ns / 1000.0; };
    out << label_ << ',' << s.count << ','
        << to_us(s.min_ns) << ','
        << to_us(s.avg_ns) << ','
        << to_us(s.stddev_ns) << ','
        << to_us(s.p50_ns) << ','
        << to_us(s.p90_ns) << ','
        << to_us(s.p99_ns) << ','
        << to_us(s.p999_ns) << ','
        << to_us(s.max_ns) << '\n';
  }

  // Human-readable single-line summary (us units).
  std::string ToHumanString() {
    auto s = Compute();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "[%s] n=%zu min=%.2fus avg=%.2fus p50=%.2fus p90=%.2fus "
             "p99=%.2fus p99.9=%.2fus max=%.2fus stddev=%.2fus",
             label_.c_str(), s.count,
             s.min_ns / 1000.0, s.avg_ns / 1000.0,
             s.p50_ns / 1000.0, s.p90_ns / 1000.0,
             s.p99_ns / 1000.0, s.p999_ns / 1000.0,
             s.max_ns / 1000.0, s.stddev_ns / 1000.0);
    return std::string(buf);
  }

 private:
  // Nearest-rank percentile. Requires samples_ already sorted ascending.
  double Percentile(double q) const {
    if (samples_.empty()) return 0.0;
    // index = ceil(q * n) - 1, clamped to [0, n-1]
    size_t n = samples_.size();
    size_t idx = static_cast<size_t>(std::ceil(q * static_cast<double>(n)));
    if (idx == 0) idx = 1;
    if (idx > n) idx = n;
    return static_cast<double>(samples_[idx - 1]);
  }

  std::string label_;
  std::vector<uint64_t> samples_;
};

}  // namespace ibmw::common::util
