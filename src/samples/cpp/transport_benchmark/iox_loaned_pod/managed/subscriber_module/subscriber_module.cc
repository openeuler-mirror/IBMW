// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iox_style_publisher.h"
// IBMW_R1_EXEMPT_END

#include "bench_payload.h"

namespace ibmw::samples::transport_benchmark::iox_loaned_pod {

namespace {
// MUST exactly match publisher_module.cc.
constexpr std::string_view kTopic = "iox_bench_pod_topic";
constexpr std::string_view kMsgType = "BenchPod";
}  // namespace

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Driver injection happens between Initialize() and Start(); only opts
  // loading + stats reservation lives in Setup. RegisterRawPodSubscriber
  // moves to Run() where driver_ is guaranteed non-null.
  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  stats_ = ibmw::common::util::LatencyStats(opts_.label);
  // Reserve room for `duration_sec * hz` plus 20% headroom so Add() is
  // amortised O(1) without vector reallocation skewing tail latency.
  size_t expected = static_cast<size_t>(opts_.duration_sec) *
                    static_cast<size_t>(opts_.hz);
  stats_.Reserve(expected + expected / 5 + 64);

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Setup OK: label={} payload_bytes={} hz={} "
         "duration_sec={} csv={}",
         opts_.label, opts_.payload_bytes, opts_.hz, opts_.duration_sec,
         opts_.csv_path.empty() ? "<none>" : opts_.csv_path);
  return true;
}

bool SubscriberModule::Run() {
  if (driver_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "SubscriberModule: iceoryx driver not injected before Run()");
    return false;
  }

  bool ok = ibmw::extensions::iceoryx_extension::RegisterRawPodSubscriber(
      driver_, kTopic, kMsgType,
      [this](const void* payload, size_t size) {
        // Compute latency BEFORE any other work to keep the timestamp
        // tight to the iceoryx Listener delivery instant.
        uint64_t recv_ns = ibmw::common::util::MonotonicNs();

        uint64_t send_ns = 0;
        uint32_t seq = 0;
        if (!ParseBenchPod(payload, size, &send_ns, &seq)) {
          dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        uint64_t latency_ns =
            (recv_ns >= send_ns) ? (recv_ns - send_ns) : 0;

        // Skip warmup samples to avoid first-publish JIT/COW penalties
        // skewing the percentile tail.
        if (seq < opts_.warmup_messages) return;

        {
          std::lock_guard<std::mutex> lk(stats_mu_);
          stats_.Add(latency_ns);
        }
        received_.fetch_add(1, std::memory_order_relaxed);
      });

  if (!ok) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "SubscriberModule: RegisterRawPodSubscriber failed");
    return false;
  }
  return true;
}

void SubscriberModule::Stop() {
  bool already = finalized_.exchange(true);
  if (already) return;

  std::lock_guard<std::mutex> lk(stats_mu_);
  uint64_t received = received_.load();
  uint64_t dropped = dropped_.load();
  std::string human = stats_.ToHumanString();

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Stop: received={} dropped={} {}",
         received, dropped, human);

  if (!opts_.csv_path.empty()) {
    BenchCsvWriter::FinalizeAndAppend(opts_.csv_path, stats_);
  }
}

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_pod
