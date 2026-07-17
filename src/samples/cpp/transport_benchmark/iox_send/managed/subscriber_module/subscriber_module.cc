// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <memory>

#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::iox_send {

namespace {
// MUST exactly match publisher_module.cc.
constexpr std::string_view kTopic = "iox_bench_send_topic";
}  // namespace

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  stats_ = ibmw::common::util::LatencyStats(opts_.label);
  size_t expected = static_cast<size_t>(opts_.duration_sec) *
                    static_cast<size_t>(opts_.hz);
  stats_.Reserve(expected + expected / 5 + 64);

  // Standard receive-side stack via per-payload typed SenderAccess /
  // ReceiverAccess wrappers. Must be wired in Setup()
  // because the transport bus rejects new subscriber/OnMessage calls
  // "after start". The iceoryx driver routes deliveries through the
  // engine MessageHandler exactly the same way as iox_loaned_ctx --
  // only the publish path differs.
  //
  // We register one typed receiver for every Plain bench type so any
  // payload size selection in publisher.yaml is honoured. Each
  // GetSubscriber(kTopic) call de-dups on topic name (the transport
  // bus returns the same ReceiverRef); each typed OnMessage adds its
  // own iceoryx receive URL keyed on (topic, msg_type). ReceiverAccess<T>
  // is immobile, so each optional slot is emplaced directly from the
  // ReceiverRef rather than via the subscriber_for<T> factory.
#define IBMW_SUB_SETUP_REGISTER(LBL, N, ALIAS)                                \
  {                                                                           \
    sub_##N##_.emplace(core_.GetTransport().GetSubscriber(kTopic));           \
    bool ok = sub_##N##_->OnMessage(                                          \
        [this](const std::shared_ptr<const ALIAS>& msg) {                     \
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
  // -- iox_send never calls extension-specific APIs.
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

}  // namespace ibmw::samples::transport_benchmark::iox_send
