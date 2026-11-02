// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Direct stable-slot Reporter V1 hot-hook benchmark used by the preregistered
// Core Metrics gate. Registration is deliberately outside timed regions.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <time.h>

#include "extension_api/ibmw_reporter_v1.h"
#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace {
constexpr uint64_t kWarmup = 2'000'000;
constexpr uint64_t kMeasured = 20'000'000;
#if !defined(__GNUC__) && !defined(__clang__)
#error "reporter_hot_bench requires a compiler with GNU-style inline assembly"
#endif
const int kMetricsOwner = 0;

template <typename T>
inline T Opaque(T value) noexcept {
  asm volatile("" : "+r"(value));
  return value;
}

__attribute__((noinline)) void Run(const ibmw_reporter_v1* api,
                                   ibmw_reporter_slot_v1* slot,
                                   uint64_t count) {
  // This is the actual ABI indirect call and stable null branch used by the
  // iceoryx extension helper after binding. LoanSerializedSample itself needs
  // live IPC and is therefore covered by E2E rather than this inner loop.
  for (uint64_t i = 0; i < count; ++i) {
    const ibmw_reporter_v1* observed = Opaque(api);
    if (observed != nullptr) observed->add(observed->owner, slot, 1);
  }
}

enum class Control { kEmpty, kNull, kNoopAbi, kAtomic };

struct LaneResult {
  uint64_t elapsed_ns{};
  double ns_per_message{};
};

uint64_t MonotonicNowNs() {
  timespec value{};
#ifdef CLOCK_MONOTONIC_RAW
  constexpr clockid_t clock = CLOCK_MONOTONIC_RAW;
#else
  constexpr clockid_t clock = CLOCK_MONOTONIC;
#endif
  if (clock_gettime(clock, &value) != 0) {
    std::cerr << "clock_gettime failed\n";
    std::exit(6);
  }
  return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1000000000) +
         static_cast<uint64_t>(value.tv_nsec);
}

__attribute__((noinline)) void NoopAdd(void* owner,
                                       ibmw_reporter_slot_v1* slot,
                                       uint64_t value) {
  asm volatile("" : : "r"(owner), "r"(slot), "r"(value) : "memory");
}

__attribute__((noinline)) void RunControl(
    Control control, const ibmw_reporter_v1* real_api,
    const std::array<ibmw_reporter_slot_v1*, 3>& slots, uint64_t count) {
  static const ibmw_reporter_v1 noop_api{
      IBMW_REPORTER_ABI_VERSION_V1, sizeof(ibmw_reporter_v1), nullptr, nullptr,
      &NoopAdd, {0}, nullptr};
  auto* noop_slot = reinterpret_cast<ibmw_reporter_slot_v1*>(
      const_cast<int*>(&kMetricsOwner));
  for (uint64_t i = 0; i < count; ++i) {
    switch (control) {
      case Control::kEmpty:
        asm volatile("" : : "r"(i) : "memory");
        break;
      case Control::kNull: {
        const ibmw_reporter_v1* first = Opaque<const ibmw_reporter_v1*>(nullptr);
        if (first != nullptr) first->add(first->owner, nullptr, 1);
        const ibmw_reporter_v1* second = Opaque<const ibmw_reporter_v1*>(nullptr);
        if (second != nullptr) second->add(second->owner, nullptr, 1);
        const ibmw_reporter_v1* third = Opaque<const ibmw_reporter_v1*>(nullptr);
        if (third != nullptr) third->add(third->owner, nullptr, 1);
        break;
      }
      case Control::kNoopAbi: {
        const ibmw_reporter_v1* api = Opaque(&noop_api);
        api->add(api->owner, noop_slot, 1);
        api->add(api->owner, noop_slot, 1);
        api->add(api->owner, noop_slot, 1);
        break;
      }
      case Control::kAtomic: {
        const ibmw_reporter_v1* api = Opaque(real_api);
        api->add(api->owner, slots[0], 1);
        api->add(api->owner, slots[1], 1);
        api->add(api->owner, slots[2], 1);
        break;
      }
    }
  }
}

bool ParseControls(std::string_view text, std::array<Control, 4>* controls,
                   std::array<std::string_view, 4>* names) {
  constexpr std::array<std::string_view, 4> expected{
      "empty", "null", "noop_abi", "atomic"};
  size_t begin = 0;
  for (size_t i = 0; i < controls->size(); ++i) {
    const size_t end = text.find(',', begin);
    if ((i != controls->size() - 1 && end == std::string_view::npos) ||
        (i == controls->size() - 1 && end != std::string_view::npos)) {
      return false;
    }
    const std::string_view name = text.substr(begin, end - begin);
    size_t index = 0;
    while (index < expected.size() && expected[index] != name) ++index;
    if (index == expected.size()) return false;
    for (size_t j = 0; j < i; ++j) {
      if ((*names)[j] == name) return false;
    }
    (*controls)[i] = static_cast<Control>(index);
    (*names)[i] = name;
    begin = end + 1;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  const bool controls_mode = argc == 3 && std::strcmp(argv[1], "--controls") == 0;
  if (argc != 1 && !controls_mode) {
    std::cerr << "usage: " << argv[0]
              << " [--controls empty,null,noop_abi,atomic]\n";
    return 1;
  }
  std::array<Control, 4> controls{};
  std::array<std::string_view, 4> control_names{};
  if (controls_mode && !ParseControls(argv[2], &controls, &control_names)) {
    std::cerr << "--controls requires exactly one permutation of "
                 "empty,null,noop_abi,atomic\n";
    return 1;
  }
  // The slot kind names the gate this run belongs to; the hook itself is the
  // same ABI call for every transport.
  std::string_view hot_kind = "direct_iceoryx_loan";
  if (const char* requested_kind = std::getenv("IBMW_REPORTER_HOT_KIND")) {
    hot_kind = requested_kind;
    if (hot_kind != "direct_iceoryx_loan" && hot_kind != "direct_shm_loan") {
      std::cerr << "IBMW_REPORTER_HOT_KIND must be direct_iceoryx_loan or "
                   "direct_shm_loan\n";
      return 1;
    }
  }
#ifdef IBMW_ENABLE_CORE_METRICS
  auto& metrics =
      ibmw::runtime::core::observability::CoreMetricsController();
  metrics.Initialize(&kMetricsOwner);
  const char* mode = std::getenv("IBMW_CORE_METRICS");
  const bool counters_requested =
      mode != nullptr && std::string_view(mode) == "counters";
  if (metrics.Enabled() != counters_requested) {
    std::cerr << "Core Metrics initialization did not select the requested mode\n";
    return 2;
  }
#endif
  // A counters run is meaningful only when the reporter ABI is actually
  // bound; an off/null run is the compile-out baseline, not instrumentation.
  const ibmw_reporter_v1* api = ibmw_reporter_query_v1();
#ifdef IBMW_ENABLE_CORE_METRICS
  if ((api != nullptr) != counters_requested) {
    std::cerr << "Reporter binding does not match the Core Metrics mode\n";
    metrics.FinalizeSnapshot(&kMetricsOwner);
    return 3;
  }
#endif
  if (controls_mode) {
#ifndef IBMW_ENABLE_CORE_METRICS
    std::cerr << "controls mode requires instrumented Core Metrics\n";
    return 3;
#else
    if (!counters_requested || api == nullptr) {
      std::cerr << "controls mode requires IBMW_CORE_METRICS=counters and a "
                   "real Reporter binding\n";
      metrics.FinalizeSnapshot(&kMetricsOwner);
      return 3;
    }
    std::array<ibmw_reporter_slot_v1*, 3> slots{};
    constexpr std::array<const char*, 3> metric_names{
        "micro_atomic_0", "micro_atomic_1", "micro_atomic_2"};
    static constexpr char kind[] = "reporter_micro_control";
    for (size_t i = 0; i < slots.size(); ++i) {
      ibmw_reporter_slot_descriptor_v1 descriptor{};
      descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
      descriptor.struct_size = sizeof(descriptor);
      descriptor.kind = {kind, sizeof(kind) - 1};
      descriptor.metric = {metric_names[i], std::strlen(metric_names[i])};
      slots[i] = api->register_slot(api->owner, &descriptor);
      if (slots[i] == nullptr) {
        std::cerr << "Reporter control slot registration failed\n";
        metrics.FinalizeSnapshot(&kMetricsOwner);
        return 4;
      }
    }
    metrics.Start(&kMetricsOwner);
    std::array<LaneResult, 4> results{};
    for (size_t position = 0; position < controls.size(); ++position) {
      const Control lane = controls[position];
      RunControl(lane, api, slots, kWarmup);
      const uint64_t start_ns = MonotonicNowNs();
      RunControl(lane, api, slots, kMeasured);
      const uint64_t elapsed_ns = MonotonicNowNs() - start_ns;
      const size_t index = static_cast<size_t>(lane);
      results[index] = {elapsed_ns,
                        static_cast<double>(elapsed_ns) / kMeasured};
    }
    const uint64_t expected_atomic = 3 * (kWarmup + kMeasured);
    uint64_t observed_atomic = 0;
    for (ibmw_reporter_slot_v1* slot : slots) {
      const auto* reporter_slot = reinterpret_cast<const ibmw::runtime::core::
          observability::Registry::ReporterSlot*>(slot);
      observed_atomic +=
          metrics.registry().ReporterValueForTesting(reporter_slot);
    }
    const bool conservation_valid = observed_atomic == expected_atomic;
    const bool controls_snapshot_valid =
        metrics.FinalizeSnapshot(&kMetricsOwner);
    if (!controls_snapshot_valid) {
      std::cerr << "Core Metrics final snapshot failed\n";
      return 5;
    }
    std::cout << std::setprecision(17) << "{\"order\":[";
    for (size_t i = 0; i < control_names.size(); ++i) {
      if (i != 0) std::cout << ',';
      std::cout << '\"' << control_names[i] << '\"';
    }
    std::cout << "],\"iterations\":" << kMeasured
              << ",\"warmup\":" << kWarmup << ",\"lanes\":{";
    constexpr std::array<std::string_view, 4> lane_names{
        "empty", "null", "noop_abi", "atomic"};
    for (size_t i = 0; i < lane_names.size(); ++i) {
      if (i != 0) std::cout << ',';
      std::cout << '\"' << lane_names[i] << "\":{\"elapsed_ns\":"
                << results[i].elapsed_ns << ",\"ns_per_message\":"
                << results[i].ns_per_message << '}';
    }
    std::cout << "},\"atomic\":{\"expected_count\":" << expected_atomic
              << ",\"observed_count\":" << observed_atomic
              << "},\"conservation_valid\":"
              << (conservation_valid ? "true" : "false") << "}\n";
    return conservation_valid ? 0 : 7;
#endif
  }
  ibmw_reporter_slot_v1* slot = nullptr;
  if (api != nullptr) {
    static constexpr char metric[] = "hot_hook";
    ibmw_reporter_slot_descriptor_v1 d{};
    d.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
    d.struct_size = sizeof(d);
    d.kind = {hot_kind.data(), hot_kind.size()};
    d.metric = {metric, sizeof(metric) - 1};
    slot = api->register_slot(api->owner, &d);
    if (slot == nullptr) {
      std::cerr << "Reporter slot registration failed\n";
#ifdef IBMW_ENABLE_CORE_METRICS
      metrics.FinalizeSnapshot(&kMetricsOwner);
#endif
      return 4;
    }
  }
#ifdef IBMW_ENABLE_CORE_METRICS
  metrics.Start(&kMetricsOwner);
#endif
  Run(api, slot, kWarmup);
  const auto begin = std::chrono::steady_clock::now();
  Run(api, slot, kMeasured);
  const auto end = std::chrono::steady_clock::now();
#ifdef IBMW_ENABLE_CORE_METRICS
  if (!metrics.FinalizeSnapshot(&kMetricsOwner)) {
    std::cerr << "Core Metrics final snapshot failed\n";
    return 5;
  }
#endif
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count();
  std::cout << "{\"iterations\":" << kMeasured << ",\"elapsed_ns\":" << ns
            << ",\"ns_per_op\":" << (static_cast<double>(ns) / kMeasured)
            << ",\"reporter_bound\":" << (api ? "true" : "false") << "}\n";
  return 0;
}
