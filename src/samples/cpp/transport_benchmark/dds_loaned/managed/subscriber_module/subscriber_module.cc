// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <memory>

#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::dds_loaned {

namespace {
// MUST exactly match publisher_module.cc.
constexpr std::string_view kTopic = "dds_bench_loaned_topic";
}  // namespace

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  stats_ = ibmw::common::util::LatencyStats(opts_.label);
  size_t expected = static_cast<size_t>(opts_.duration_sec) *
                    static_cast<size_t>(opts_.hz);
  stats_.Reserve(expected + expected / 5 + 64);

  // Standard receive-side stack: GetSubscriber + OnMessage<T>. Must be
  // wired in Setup() because the transport bus rejects new
  // GetSubscriber/OnMessage calls "after start". The dds driver routes
  // deliveries through the engine MessageHandler exactly the same way
  // as dds_send -- only the publish path differs (LoanedMessage vs
  // standard Send).
  //
  // We register one OnMessage<PlainT> for every Plain bench type so any
  // payload size selection in publisher.yaml is honoured. The shared
  // GetSubscriber(kTopic) call returns the same SubscriberRef each time
  // (the transport bus de-dups on topic name); each typed OnMessage
  // adds its own DDS DataReader keyed on (topic, msg_type).
  auto subscriber = core_.GetTransport().GetSubscriber(std::string(kTopic));

#define IBMW_SUB_SETUP_REGISTER(LBL, N, ALIAS)                                \
  {                                                                           \
    bool ok = ibmw::transport::OnMessage<ALIAS>(                              \
        subscriber,                                                           \
        [this](const std::shared_ptr<const ALIAS>& msg) {                     \
          /* Capture recv_ns immediately to keep the gap to the engine */    \
          /* delivery instant tight; defer all bookkeeping until after. */    \
          uint64_t recv_ns = ibmw::common::util::MonotonicNs();               \
          if (!msg) {                                                         \
            dropped_.fetch_add(1, std::memory_order_relaxed);                 \
            return;                                                           \
          }                                                                   \
          uint64_t send_ns = ExtractBenchPlainSendNs(*msg);                   \
          uint32_t seq     = ExtractBenchPlainSeq(*msg);                      \
          if (send_ns == 0) {                                                 \
            dropped_.fetch_add(1, std::memory_order_relaxed);                 \
            return;                                                           \
          }                                                                   \
          uint64_t latency_ns =                                               \
              (recv_ns >= send_ns) ? (recv_ns - send_ns) : 0;                 \
          /* Skip warmup samples to avoid first-publish JIT/COW   */          \
          /* penalties skewing the percentile tail.                */         \
          if (seq < opts_.warmup_messages) return;                            \
          {                                                                   \
            std::lock_guard<std::mutex> lk(stats_mu_);                        \
            stats_.Add(latency_ns);                                           \
          }                                                                   \
          uint64_t received =                                                 \
              received_.fetch_add(1, std::memory_order_relaxed) + 1;           \
          bool expected = false;                                               \
          if (first_sample_logged_.compare_exchange_strong(                    \
                  expected, true, std::memory_order_acq_rel)) {                \
            MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,       \
                   "SubscriberModule first sample: payload_bytes={} seq={} "  \
                   "received={} latency_ns={}",                               \
                   opts_.payload_bytes, seq, received, latency_ns);            \
          }                                                                    \
        });                                                                   \
    if (!ok) {                                                                \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,           \
             "SubscriberModule: OnMessage<" LBL "> failed");                  \
      return false;                                                           \
    }                                                                         \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SUB_SETUP_REGISTER)
#undef IBMW_SUB_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} csv={}",
         opts_.label, opts_.payload_bytes,
         PayloadSizeLabel(opts_.payload_bytes)
             ? PayloadSizeLabel(opts_.payload_bytes)
             : "<unsupported>",
         opts_.hz, opts_.duration_sec,
         opts_.csv_path.empty() ? "<none>" : opts_.csv_path);
  return true;
}

bool SubscriberModule::Run() {
  // Nothing to do at Run(): deliveries already flow through the
  // OnMessage handlers installed in Setup(). No driver pointer needed
  // -- dds_loaned subscriber never calls extension-specific APIs.
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

}  // namespace ibmw::samples::transport_benchmark::dds_loaned
