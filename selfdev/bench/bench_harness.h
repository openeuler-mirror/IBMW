// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Minimal chrono-based benchmark harness shared by the selfdev benchmarks.
// Each case runs `warmup` untimed repetitions, then `reps` timed ones. A
// repetition returns the number of operations it performed; the harness
// reports ns/op (median, p10, p90 over repetitions) and ops/s at the median.
// Compare() warms up both variants, then interleaves their repetitions.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <sys/resource.h>

namespace ibmw::selfdev::bench {

struct Options {
  int warmup = 1;
  int reps = 7;
  double scale = 1.0;  // multiplies per-case operation counts
  std::string filter;  // substring of the case name; empty runs all
};

inline Options ParseOptions(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strncmp(a, "--reps=", 7) == 0) {
      o.reps = std::max(1, std::atoi(a + 7));
    } else if (std::strncmp(a, "--warmup=", 9) == 0) {
      o.warmup = std::max(0, std::atoi(a + 9));
    } else if (std::strncmp(a, "--scale=", 8) == 0) {
      o.scale = std::atof(a + 8);
      if (o.scale <= 0) o.scale = 1.0;
    } else if (std::strncmp(a, "--filter=", 9) == 0) {
      o.filter = a + 9;
    } else {
      std::fprintf(stderr, "usage: %s [--reps=N] [--warmup=N] [--scale=F] [--filter=SUBSTR]\n", argv[0]);
      std::exit(2);
    }
  }
  return o;
}

/// What one repetition reports. A body that has setup work it does not want
/// timed measures its own region and sets `ns`; otherwise the harness times
/// the whole call.
struct Sample {
  Sample(uint64_t o) : ops(o) {}  // NOLINT(google-explicit-constructor)
  Sample(uint64_t o, double n) : ops(o), ns(n) {}
  uint64_t ops;
  double ns = -1;
};

/// Stopwatch for bodies that time their own region.
class Stopwatch {
 public:
  Stopwatch() : t0_(std::chrono::steady_clock::now()) {}
  double ElapsedNs() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count());
  }

 private:
  std::chrono::steady_clock::time_point t0_;
};

struct Result {
  double median_ns = 0;
  double p10_ns = 0;
  double p90_ns = 0;
  // Median voluntary context switches (thread sleeps) per 1000 ops.
  double csw_per_kop = 0;
};

/// Voluntary context switches of the whole process so far (all threads,
/// including ones that have exited).
inline double VoluntaryCsw() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_nvcsw);
}

inline double Percentile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(lo + 1, v.size() - 1);
  const double frac = pos - static_cast<double>(lo);
  return v[lo] * (1.0 - frac) + v[hi] * frac;
}

class Runner {
 public:
  explicit Runner(Options o) : opt_(std::move(o)) {
    std::printf("%-58s %12s %12s %12s %14s %10s\n", "case", "median ns/op", "p10", "p90", "ops/s(median)",
                "csw/kop");
  }

  const Options& options() const { return opt_; }

  /// Scales a nominal operation count, never below 1.
  uint64_t N(uint64_t nominal) const {
    const double v = static_cast<double>(nominal) * opt_.scale;
    return v < 1 ? 1 : static_cast<uint64_t>(v);
  }

  bool Selected(const std::string& name) const {
    return opt_.filter.empty() || name.find(opt_.filter) != std::string::npos;
  }

  /// `body` performs one repetition and returns its operation count.
  Result Run(const std::string& name, const std::function<Sample()>& body) {
    if (!Selected(name)) return {};
    for (int i = 0; i < opt_.warmup; ++i) (void)body();
    Series s(opt_.reps);
    for (int i = 0; i < opt_.reps; ++i) s.Add(body);
    return s.Report(name);
  }

  /// Runs the selfdev and original variants of one scenario and prints the
  /// median ratio (selfdev / original; < 1 means selfdev is faster). The
  /// repetitions alternate, AB BA AB ..., so that a drift over the run (CPU
  /// frequency, migration to another core, heap growth) does not fall on
  /// one side: on the OHOS board the side run first was up to 1.5x slower.
  void Compare(const std::string& scenario, const std::function<Sample()>& selfdev,
               const std::function<Sample()>& original, const char* original_name) {
    if (!Selected(scenario)) return;
    for (int i = 0; i < opt_.warmup; ++i) {
      (void)selfdev();
      (void)original();
    }
    Series a(opt_.reps);
    Series b(opt_.reps);
    for (int i = 0; i < opt_.reps; ++i) {
      if (i % 2 == 0) {
        a.Add(selfdev);
        b.Add(original);
      } else {
        b.Add(original);
        a.Add(selfdev);
      }
    }
    const Result ra = a.Report(scenario + " [selfdev]");
    const Result rb = b.Report(scenario + " [" + original_name + "]");
    std::printf("%-58s ratio selfdev/%s = %.2f\n", scenario.c_str(), original_name,
                rb.median_ns > 0 ? ra.median_ns / rb.median_ns : 0.0);
    std::fflush(stdout);
  }

 private:
  // Timed repetitions of one case.
  class Series {
   public:
    explicit Series(int reps) {
      ns_per_op_.reserve(static_cast<size_t>(reps));
      csw_per_kop_.reserve(static_cast<size_t>(reps));
    }

    void Add(const std::function<Sample()>& body) {
      const double csw0 = VoluntaryCsw();
      const Stopwatch sw;
      const Sample s = body();
      const double ns = s.ns >= 0 ? s.ns : sw.ElapsedNs();
      const double ops = static_cast<double>(s.ops == 0 ? 1 : s.ops);
      ns_per_op_.push_back(ns / ops);
      csw_per_kop_.push_back((VoluntaryCsw() - csw0) * 1000.0 / ops);
    }

    Result Report(const std::string& name) const {
      Result r;
      r.median_ns = Percentile(ns_per_op_, 0.5);
      r.p10_ns = Percentile(ns_per_op_, 0.1);
      r.p90_ns = Percentile(ns_per_op_, 0.9);
      r.csw_per_kop = Percentile(csw_per_kop_, 0.5);
      std::printf("%-58s %12.1f %12.1f %12.1f %14.0f %10.2f\n", name.c_str(), r.median_ns, r.p10_ns, r.p90_ns,
                  r.median_ns > 0 ? 1e9 / r.median_ns : 0.0, r.csw_per_kop);
      std::fflush(stdout);
      return r;
    }

   private:
    std::vector<double> ns_per_op_;
    std::vector<double> csw_per_kop_;
  };

  Options opt_;
};

/// Keeps `v` alive so the optimizer cannot drop the computation producing it.
template <class T>
inline void DoNotOptimize(T const& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}

}  // namespace ibmw::selfdev::bench
