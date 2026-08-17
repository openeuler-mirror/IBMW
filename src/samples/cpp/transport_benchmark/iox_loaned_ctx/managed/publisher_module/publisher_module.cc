// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"
#include "pacing_schedule.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {

namespace {
// Single shared topic name across all 5 typed publishers. Iceoryx
// disambiguates services by (topic, msg_type) -- since each Plain bench
// type has a distinct IBMW_DDS_TYPE_NAME the URLs are unique despite the
// shared topic. This also keeps the yaml routing rule simple (one
// `enable_drivers: [iceoryx]` line for the whole bench) and matches the
// publisher/subscriber convention from the original CompressedImage
// implementation -- only the type bank changed.
constexpr std::string_view kTopic = "iox_bench_ctx_topic";

// The msg_type strings MUST match what the engine catalog records for
// each DDS type. The DDS type-support layer prefixes every type with
// "dds:" (see dds_type_support.h:78), so the iceoryx driver looks up
// /transport/<topic>/dds%3A<TypeName> -- not /<topic>/<TypeName>.
//
// IBMW_DDS_TYPE_NAME(ALIAS, "ibmw::samples::transport_benchmark::ALIAS")
// is exactly what bench_plain_payloads.h registers, so we mirror that
// here to compose the prefixed lookup key.
template <typename PlainT>
inline std::string CtxMsgType();

#define IBMW_PUB_MSG_TYPE_FN(LBL, N, ALIAS)                              \
  template <>                                                            \
  inline std::string CtxMsgType<ALIAS>() {                               \
    return std::string("dds:") +                                         \
           "ibmw::samples::transport_benchmark::BenchPlainPayload" LBL;    \
  }
IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_MSG_TYPE_FN)
#undef IBMW_PUB_MSG_TYPE_FN
}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // Driver is injected between Initialize() and Start(); only opts loading
  // lives in Setup. Driver-bound resources are created in Run().
  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  if (PayloadSizeLabel(opts_.payload_bytes) == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: payload_bytes={} not in supported bench "
           "matrix (1024/4096/16384/65536/262144/1048576/4194304)",
           opts_.payload_bytes);
    return false;
  }

  // Step 1 (Setup time): the standard transport bus rejects new
  // GetPublisher/RegisterSendType calls "after start", so we must
  // claim ALL 5 typed (topic, msg_type) entries in iceoryx_bus_ here,
  // while the engine is still in the wiring phase. Even slots we won't
  // actually publish from this run need their URL pre-registered,
  // because we cannot prove at Setup time which one a future yaml will
  // pick (and the cost of an idle publisher URL is negligible). The
  // iceoryx driver populates its publisher table from the same
  // RegisterSendType callback, so by Run() time
  // IoxCtxMetaTypedPublisher<T> can find the publisher via
  // iceoryx_bus_.GetPublisher(url).
#define IBMW_PUB_SETUP_REGISTER(LBL, N, ALIAS)                                 \
  {                                                                            \
    auto& slot = slot_##N##_;                                                  \
    slot.sender = core_.GetTransport().GetPublisher(std::string(kTopic));      \
    if (!ibmw::transport::RegisterSendType<ALIAS>(slot.sender)) {              \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: RegisterSendType<" LBL "> failed");             \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_SETUP_REGISTER)
#undef IBMW_PUB_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} warmup={}",
         opts_.label, opts_.payload_bytes,
         PayloadSizeLabel(opts_.payload_bytes), opts_.hz, opts_.duration_sec,
         opts_.warmup_messages);
  return true;
}

bool PublisherModule::Run() {
  if (driver_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: iceoryx driver not injected before Run()");
    return false;
  }

  // Build all 5 typed loaned-publishers around the driver. We create
  // every slot up-front (cheap -- each one just caches a TransportInfo)
  // so PublishLoop's inner dispatch never needs a lazy-init branch.
  // From now on, the chosen slot's publish path is:
  //   borrow chunk -> CDR-serialize-into-chunk -> emit ctx-meta header ->
  //   PublishLoaned, no heap copy.
#define IBMW_PUB_RUN_CREATE(LBL, N, ALIAS)                                     \
  {                                                                            \
    auto& slot = slot_##N##_;                                                  \
    slot.typed_pub = ibmw::extensions::iceoryx_extension::                     \
        CreateCtxMetaTypedPublisher<ALIAS>(driver_, kTopic,                    \
                                            CtxMsgType<ALIAS>());              \
    if (slot.typed_pub == nullptr) {                                           \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: CreateCtxMetaTypedPublisher<" LBL "> failed");  \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_RUN_CREATE)
#undef IBMW_PUB_RUN_CREATE

  running_ = true;
  thread_ = std::thread([this]() { PublishLoop(); });
  std::cout << "PERF_PUBLISHER_RUNNING" << std::endl;
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (!opts_.publisher_status_path.empty()) {
    std::ofstream out(opts_.publisher_status_path, std::ios::trunc);
    if (out) {
      const bool valid = completed_send_window_ && measured_attempts_ > 0 &&
                         measured_fail_ == 0 &&
                         measured_success_ == measured_attempts_;
      out << "{\n  \"measure_begin_monotonic_ns\": " << opts_.measure_begin_monotonic_ns
          << ",\n  \"measure_end_monotonic_ns\": " << opts_.measure_end_monotonic_ns
          << ",\n  \"attempts\": " << measured_attempts_ << ",\n  \"success\": " << measured_success_
          << ",\n  \"fail\": " << measured_fail_ << ",\n  \"first_attempt_seq\": " << first_attempt_seq_
          << ",\n  \"last_attempt_seq\": " << last_attempt_seq_ << ",\n  \"first_success_seq\": " << first_success_seq_
          << ",\n  \"last_success_seq\": " << last_success_seq_
          << ",\n  \"completed_send_window\": "
          << (completed_send_window_ ? "true" : "false")
          << ",\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    }
  }
#define IBMW_PUB_STOP_RESET(LBL, N, ALIAS) slot_##N##_.typed_pub.reset();
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_STOP_RESET)
#undef IBMW_PUB_STOP_RESET
}

void PublisherModule::PublishLoop() {
  using clock = std::chrono::steady_clock;

  const uint32_t batch = std::max<uint32_t>(opts_.batch_size, 1);
  const auto batch_period_ns = BatchPacingPeriod(opts_.hz, batch);
  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);
  const bool shared_window = opts_.measure_begin_monotonic_ns != 0;
  const auto begin = clock::time_point(std::chrono::nanoseconds(opts_.measure_begin_monotonic_ns));
  const auto end = clock::time_point(std::chrono::nanoseconds(opts_.measure_end_monotonic_ns));
  const auto send_begin = begin - std::chrono::seconds(opts_.warmup_sec);
  const auto send_end = end + std::chrono::seconds(opts_.duration_sec - opts_.warmup_sec - opts_.measured_sec);

  // The CDR sertype is the only KV the receive side needs to
  // disambiguate the payload encoding. Setting it explicitly here keeps
  // the publish path off the "default sertype lookup" branch in
  // LoanSerializedSampleWithCtx and matches what the standard Send()
  // loop would emit, so subscribers see byte-identical headers across
  // iox_send and iox_loaned_ctx (apart from the loan vs allocator path).
  ibmw::transport::Context ctx;
  ctx.SetSerializationType("cdr");

  uint32_t seq = 0;
  uint64_t publish_failed = 0;
  // Keep an absolute schedule. Resetting an overdue tick to now creates
  // catch-up bursts whose CPU load depends on scheduler jitter.
  auto next_tick = shared_window ? send_begin : clock::now();
  if (shared_window && clock::now() < send_begin) {
    std::this_thread::sleep_until(send_begin);
  }

  while (running_ && clock::now() < (shared_window ? send_end : deadline)) {
    const uint32_t this_batch = batch;
    for (uint32_t batch_pos = 0;
         batch_pos < this_batch && running_ && clock::now() < (shared_window ? send_end : deadline);
         ++batch_pos) {
    // Static fan-out by payload_bytes. The if-chain is fully resolved
    // at compile time per branch -- the only runtime cost over the
    // single-type loop is one `==` per iteration per non-matching
    // branch (negligible: 5 cmps at most for the largest bank).
#define IBMW_PUB_LOOP_DISPATCH(LBL, N, ALIAS)                                  \
  if (opts_.payload_bytes == (N)) {                                            \
    /* Heap-allocate the message: 4 MB on the stack would blow past   */       \
    /* the default 8 MB pthread stack on the publisher thread.        */       \
    auto msg = std::make_unique<ALIAS>();                                      \
    FillBenchPlainPayload(*msg, seq);                                          \
    const uint64_t send_ns = ExtractBenchPlainSendNs(*msg);                    \
    const bool measured = shared_window && send_ns >= opts_.measure_begin_monotonic_ns && send_ns < opts_.measure_end_monotonic_ns; \
    if (measured) { if (!have_attempt_) first_attempt_seq_ = seq; last_attempt_seq_ = seq; have_attempt_ = true; ++measured_attempts_; } \
    if (!slot_##N##_.typed_pub->publish(ctx, *msg)) {                          \
      ++publish_failed;                                                        \
      if (measured) ++measured_fail_;                                          \
    } else if (measured) {                                                     \
      if (!have_success_) first_success_seq_ = seq; last_success_seq_ = seq; have_success_ = true; ++measured_success_; \
    }                                                                          \
  } else
    IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_LOOP_DISPATCH)
    {
      // Caught by the Setup() validation; defensive only.
      ++publish_failed;
    }
#undef IBMW_PUB_LOOP_DISPATCH
      ++seq;
    }
    // Fixed-batch open-loop pacing keeps scheduler work distinguishable from
    // transport work and removes the old per-message sleep dependency.
    if (opts_.hz != 0) {
      next_tick += batch_period_ns;
      auto now = clock::now();
      next_tick = AdvancePacingDeadline<clock>(next_tick, batch_period_ns, now);
      std::this_thread::sleep_until(next_tick);
    }
  }
  completed_send_window_ =
      clock::now() >= (shared_window ? send_end : deadline);

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={} publish_failed={}",
         seq, publish_failed);
}

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
