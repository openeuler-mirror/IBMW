// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <memory>

#include "bench_send_payloads.h"

namespace ibmw::samples::transport_benchmark::dds_send_tcp {

namespace {
// MUST exactly match publisher_module.cc.
constexpr std::string_view kTopic = "dds_bench_send_tcp_topic";

using dds_send_bank::SendPayloadSizeLabel;
using dds_send_bank::ExtractBenchSendNs;
using dds_send_bank::ExtractBenchSendSeq;
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
  // GetSubscriber/OnMessage calls "after start". The dds driver
  // routes deliveries through the engine MessageHandler exactly the
  // same way as dds_loaned -- only the publish path differs.
  //
  // We register one OnMessage<PlainT> for every Plain bench type so
  // any payload size selection in publisher.yaml is honoured. The
  // shared GetSubscriber(kTopic) call returns the same SubscriberRef
  // each time (the transport bus de-dups on topic name); each typed
  // OnMessage adds its own DDS DataReader keyed on (topic, msg_type).
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
          uint64_t send_ns = ExtractBenchSendNs(*msg);                        \
          uint32_t seq     = ExtractBenchSendSeq(*msg);                       \
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
          received_.fetch_add(1, std::memory_order_relaxed);                  \
        });                                                                   \
    if (!ok) {                                                                \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,           \
             "SubscriberModule: OnMessage<" LBL "> failed");                  \
      return false;                                                           \
    }                                                                         \
  }
  IBMW_BENCH_SEND_FOR_EACH(IBMW_SUB_SETUP_REGISTER)
#undef IBMW_SUB_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} csv={}",
         opts_.label, opts_.payload_bytes,
         SendPayloadSizeLabel(opts_.payload_bytes)
             ? SendPayloadSizeLabel(opts_.payload_bytes)
             : "<unsupported>",
         opts_.hz, opts_.duration_sec,
         opts_.csv_path.empty() ? "<none>" : opts_.csv_path);
  return true;
}

bool SubscriberModule::Run() {
  // Nothing to do at Run(): deliveries already flow through the
  // OnMessage handlers installed in Setup(). No driver pointer needed
  // -- dds_send_tcp never calls extension-specific APIs.
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

}  // namespace ibmw::samples::transport_benchmark::dds_send_tcp
