// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <fstream>
#include <memory>

#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::iox_send {

namespace {
// MUST exactly match publisher_module.cc.
constexpr std::string_view kDefaultTopic = "iox_bench_send_topic";
}  // namespace

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  const std::string topic = opts_.topic.empty() ? std::string(kDefaultTopic) : opts_.topic;
  stats_ = ibmw::common::util::LatencyStats(opts_.label);
  size_t expected = static_cast<size_t>(opts_.duration_sec) *
                     static_cast<size_t>(opts_.hz);
  if (opts_.message_count != 0) expected = opts_.message_count;
  stats_.Reserve(expected + expected / 5 + 64);
  accounting_.emplace(opts_.message_count, opts_.warmup_messages);

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
    sub_##N##_.emplace(core_.GetTransport().GetSubscriber(topic));           \
    bool ok = sub_##N##_->OnMessage(                                          \
        [this](const std::shared_ptr<const ALIAS>& msg) {                     \
          uint64_t recv_ns = ibmw::common::util::MonotonicNs();               \
          if (!msg) {                                                         \
            std::lock_guard<std::mutex> lk(stats_mu_);                        \
            accounting_->RecordInvalidPayload();                              \
            return;                                                           \
          }                                                                   \
          uint64_t send_ns = ExtractBenchPlainSendNs(*msg);                   \
          uint32_t seq     = ExtractBenchPlainSeq(*msg);                      \
          bool payload_valid =                                                \
              msg->header.payload_bytes == ALIAS::kPayloadBytes &&            \
              msg->header.payload_bytes == opts_.payload_bytes;               \
          for (uint8_t byte : msg->filler) {                                  \
            if (byte != kBenchPlainFillerMagic) { payload_valid = false; break; }\
          }                                                                   \
          std::lock_guard<std::mutex> lk(stats_mu_);                          \
          const auto result = accounting_->Observe(                           \
              seq, send_ns != 0 && recv_ns >= send_ns, payload_valid);        \
          if (result != SequenceResult::kAccepted) return;                    \
          stats_.Add(recv_ns - send_ns);                                      \
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
  const uint64_t received = accounting_->Received();
  uint64_t dropped =
      accounting_->InvalidPayload() + accounting_->InvalidTimestamp();
  const uint64_t unique_valid = accounting_->Unique();
  const uint64_t missing = accounting_->Missing();
  const bool fixed_count_valid =
      accounting_->StrictRunValid();
  const bool valid = accounting_->IsFixedCount()
                         ? fixed_count_valid
                         : stats_.Count() != 0 &&
                               accounting_->InvalidPayload() == 0 &&
                               accounting_->InvalidTimestamp() == 0;
  std::string human = stats_.ToHumanString();

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Stop: received={} dropped={} {}",
         received, dropped, human);

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "BENCH_STATUS attempted_measured={} received_measured={} unique_measured={} missing={} duplicate={} out_of_order={} invalid_payload={} invalid_timestamp={} valid={}",
         opts_.message_count, received, unique_valid, missing,
         accounting_->Duplicate(), accounting_->OutOfOrder(),
         accounting_->InvalidPayload(), accounting_->InvalidTimestamp(), valid);
  if (!opts_.status_path.empty()) {
    std::ofstream status(opts_.status_path, std::ios::trunc);
    if (status) {
      status << "{\n"
             << "  \"attempted_total\": "
             << (static_cast<uint64_t>(opts_.warmup_messages) + opts_.message_count) << ",\n"
             << "  \"attempted_measured\": " << opts_.message_count << ",\n"
             << "  \"received_measured\": " << received << ",\n"
             << "  \"unique_measured\": " << unique_valid << ",\n"
             << "  \"missing\": " << missing << ",\n"
             << "  \"duplicate\": " << accounting_->Duplicate() << ",\n"
             << "  \"out_of_order\": " << accounting_->OutOfOrder() << ",\n"
             << "  \"invalid_payload\": " << accounting_->InvalidPayload() << ",\n"
             << "  \"invalid_timestamp\": " << accounting_->InvalidTimestamp() << ",\n"
             << "  \"valid\": " << (valid ? "true" : "false") << "\n"
             << "}\n";
    } else {
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
             "SubscriberModule: cannot write status_path={}", opts_.status_path);
    }
  }
  if (ShouldAppendCsv(opts_.message_count, fixed_count_valid) &&
      !opts_.csv_path.empty()) {
    BenchCsvWriter::FinalizeAndAppend(opts_.csv_path, stats_);
  }
}

}  // namespace ibmw::samples::transport_benchmark::iox_send
