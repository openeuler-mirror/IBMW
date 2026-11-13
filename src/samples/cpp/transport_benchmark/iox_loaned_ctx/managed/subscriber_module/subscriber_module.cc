// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <memory>
#include <fstream>
#include <iostream>

#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {

namespace {
// MUST exactly match publisher_module.cc. bench_options.topic overrides it for
// lanes that build this module against another publisher.
constexpr std::string_view kTopic = "iox_bench_ctx_topic";
}  // namespace

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  stats_ = ibmw::common::util::LatencyStats(opts_.label);
  size_t expected = static_cast<size_t>(opts_.duration_sec) *
                    static_cast<size_t>(opts_.hz ? opts_.hz : 100000);
  stats_.Reserve(expected + expected / 5 + 64);

  // Standard receive-side stack: GetSubscriber + OnMessage<T>. Must be
  // wired in Setup() because the transport bus rejects new
  // GetSubscriber/OnMessage calls "after start". The iceoryx driver
  // routes deliveries through the engine MessageHandler exactly like
  // iox_send does.
  //
  // We register one OnMessage<PlainT> for every Plain bench type so any
  // payload size selection in publisher.yaml is honoured. The shared
  // GetSubscriber(kTopic) call returns the same SubscriberRef each time
  // (the transport bus de-dups on topic name); each typed OnMessage adds
  // its own iceoryx receive URL keyed on (topic, msg_type).
  auto subscriber = core_.GetTransport().GetSubscriber(
      opts_.topic.empty() ? std::string(kTopic) : opts_.topic);

#define IBMW_SUB_SETUP_REGISTER(LBL, N, ALIAS)                                 \
  {                                                                            \
    bool ok = ibmw::transport::OnMessage<ALIAS>(                               \
        subscriber,                                                            \
        [this](const std::shared_ptr<const ALIAS>& msg) {                      \
          /* Capture recv_ns immediately to keep the gap to the engine */     \
          /* delivery instant tight; defer all bookkeeping until after. */    \
          uint64_t recv_ns = ibmw::common::util::MonotonicNs();                \
          if (!msg) {                                                          \
            dropped_.fetch_add(1, std::memory_order_relaxed);                  \
            return;                                                            \
          }                                                                    \
          uint64_t send_ns = ExtractBenchPlainSendNs(*msg);                    \
          uint32_t seq     = ExtractBenchPlainSeq(*msg);                       \
          if (send_ns == 0) {                                                  \
            dropped_.fetch_add(1, std::memory_order_relaxed);                  \
            return;                                                            \
          }                                                                    \
          uint64_t latency_ns =                                                \
              (recv_ns >= send_ns) ? (recv_ns - send_ns) : 0;                  \
          if (opts_.measure_begin_monotonic_ns != 0) {                         \
            if (send_ns < opts_.measure_begin_monotonic_ns || send_ns >= opts_.measure_end_monotonic_ns) return; \
          } else if (opts_.measured_sec != 0) {                                \
            const auto now = std::chrono::steady_clock::now();                 \
            if (now < measure_begin_ || now >= measure_end_) return;           \
          } else if (seq < opts_.warmup_messages) return;                      \
          {                                                                    \
            std::lock_guard<std::mutex> lk(stats_mu_);                         \
            if (have_last_sequence_ && seq <= last_sequence_) { ++out_of_order_; ++duplicate_; } \
            else { if (!have_last_sequence_) first_sequence_ = seq; ++unique_; } \
            if (!have_last_sequence_ && opts_.measured_sec == 0 &&             \
                seq > opts_.warmup_messages)                                   \
              missing_ += seq - opts_.warmup_messages;                         \
            if (have_last_sequence_ && seq > last_sequence_ + 1)               \
              missing_ += seq - last_sequence_ - 1;                            \
            last_sequence_ = seq;                                              \
            have_last_sequence_ = true;                                        \
            stats_.Add(latency_ns);                                            \
          }                                                                    \
          received_.fetch_add(1, std::memory_order_relaxed);                   \
        });                                                                    \
    if (!ok) {                                                                 \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "SubscriberModule: OnMessage<" LBL "> failed");                   \
      return false;                                                            \
    }                                                                          \
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
  if (driver_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "SubscriberModule: transport driver not injected before Run()");
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  measure_begin_ = now + std::chrono::seconds(opts_.warmup_sec);
  measure_end_ = measure_begin_ + std::chrono::seconds(opts_.measured_sec);
  std::cout << "PERF_SUBSCRIBER_RUNNING" << std::endl;
  // No driver-side wiring needed: deliveries already flow through the
  // OnMessage handlers installed in Setup().
  return true;
}

void SubscriberModule::Stop() {
  bool already = finalized_.exchange(true);
  if (already) return;

  std::lock_guard<std::mutex> lk(stats_mu_);
  uint64_t received = received_.load();
  uint64_t dropped = dropped_.load();
  const uint64_t unique = unique_;
  const uint64_t span_missing = have_last_sequence_ ?
      static_cast<uint64_t>(last_sequence_) - first_sequence_ + 1 - unique : 0;
  const bool valid = unique > 0 && duplicate_ == 0 && out_of_order_ == 0 &&
                     missing_ == 0 && span_missing == 0 && dropped == 0;
  std::string human = stats_.ToHumanString();

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Stop: received={} dropped={} {}",
         received, dropped, human);

  if (!opts_.status_path.empty()) {
    std::ofstream status(opts_.status_path, std::ios::trunc);
    if (status) {
      status << "{\n  \"received_measured\": " << received
             << ",\n  \"measure_begin_monotonic_ns\": " << opts_.measure_begin_monotonic_ns
             << ",\n  \"measure_end_monotonic_ns\": " << opts_.measure_end_monotonic_ns
             << ",\n  \"unique_measured\": " << unique
             << ",\n  \"duplicate\": " << duplicate_
             << ",\n  \"out_of_order\": " << out_of_order_
             << ",\n  \"missing\": " << missing_
             << ",\n  \"span_missing\": " << span_missing
             << ",\n  \"first_seq\": " << first_sequence_
             << ",\n  \"last_seq\": " << last_sequence_
             << ",\n  \"invalid\": " << dropped
             << ",\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    }
  }

  if (!opts_.csv_path.empty()) {
    BenchCsvWriter::FinalizeAndAppend(opts_.csv_path, stats_);
  }
}

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
