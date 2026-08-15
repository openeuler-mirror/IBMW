// Direct stable-slot Reporter V1 hot-hook benchmark used by the preregistered
// Core Metrics gate. Registration is deliberately outside timed regions.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

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
#ifdef IBMW_ENABLE_CORE_METRICS
const int kMetricsOwner = 0;
#endif

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
}  // namespace

int main() {
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
  const ibmw_reporter_v1* api = ibmw_reporter_query_v1();
#ifdef IBMW_ENABLE_CORE_METRICS
  if ((api != nullptr) != counters_requested) {
    std::cerr << "Reporter binding does not match the Core Metrics mode\n";
    metrics.FinalizeSnapshot(&kMetricsOwner);
    return 3;
  }
#endif
  ibmw_reporter_slot_v1* slot = nullptr;
  if (api != nullptr) {
    static constexpr char kind[] = "direct_iceoryx_loan";
    static constexpr char metric[] = "hot_hook";
    ibmw_reporter_slot_descriptor_v1 d{};
    d.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
    d.struct_size = sizeof(d);
    d.kind = {kind, sizeof(kind) - 1};
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
