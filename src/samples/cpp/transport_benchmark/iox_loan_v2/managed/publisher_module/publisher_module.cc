// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>
#include <string>

#include "internal/typesupport/dds/dds_transport.h"

namespace ibmw::samples::transport_benchmark::iox_loan_v2 {

namespace {

// Topic is distinct from the other three baselines so all four samples
// can coexist on the same RouDi. The POD wire URL the iceoryx driver
// builds is "/transport_zc_pod/<topic>/<msg_type>" (MakePodUrl); the
// dispatcher-side SendTypeWrapper lookup key must match the msg_type
// reported by the DDS TypeSupport (i.e. "dds:" + DdsTypeName<T>::get()).
//
// Phase 7 §7.3 Step 3.8: user-level loan fast lane, apples-to-apples
// with iox_send. The only per-tick difference from iox_send is:
//   * iox_send    : make_unique<T>() on heap + Send() -> CDR serialize
//                   into heap BufferArray + driver memcpy into SHM chunk
//   * iox_loan_v2 : LoanedSample<T>::Borrow() -> driver hands back SHM
//                   chunk directly; user writes T in place; Publish()
//                   flushes. No heap alloc, no memcpy.
// Subscriber path (raw POD callback reading BenchPlainHeader from the
// first 24 B of the chunk) is byte-equivalent to iox_loaned_pod's.
constexpr std::string_view kTopic = "iox_bench_loan_v2_topic";

// Build the msg_type string the dispatcher will look up in the catalog.
// Must match what RegisterSendType<T>() registered -- see
// GetDdsMessageTypeSupport() in dds_type_support.h (kMsgTypeName =
// "dds:" + DdsTypeName<T>::get()).
template <typename T>
std::string MakeDdsMsgType() {
  return std::string("dds:") + ibmw::dds::DdsTypeName<T>::get();
}

}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  if (PayloadSizeLabel(opts_.payload_bytes) == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: payload_bytes={} not in supported bench "
           "matrix (1024/4096/16384/65536/262144/1048576/4194304)",
           opts_.payload_bytes);
    return false;
  }

  // Claim ALL 5 typed (topic, msg_type) slots in Setup() -- identical
  // shape to iox_send so the only row-vs-row difference is the publish
  // call. RegisterSendType<T> is what populates the SendTypeWrapper in
  // the transport catalog that dispatcher.Loan() will later look up.
#define IBMW_LOAN_V2_SETUP_REGISTER(LBL, N, ALIAS)                             \
  {                                                                            \
    sender_##N##_ = core_.GetTransport().GetPublisher(std::string(kTopic));    \
    if (!ibmw::transport::RegisterSendType<ALIAS>(sender_##N##_)) {            \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: RegisterSendType<" LBL "> failed");             \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_LOAN_V2_SETUP_REGISTER)
#undef IBMW_LOAN_V2_SETUP_REGISTER

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

  // One-time POD topic registration for EACH type on the iceoryx side.
  // This populates pod_pub_map_ (keyed on topic + msg_type) so the hot
  // path LoanSample() lookup succeeds. Not on per-tick path -- does not
  // bias the latency comparison.
#define IBMW_LOAN_V2_RUN_ENSUREPOD(LBL, N, ALIAS)                              \
  {                                                                            \
    const std::string msg_type = MakeDdsMsgType<ALIAS>();                      \
    if (!driver_->EnsurePodTopic(kTopic, msg_type, (N))) {                     \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: EnsurePodTopic<" LBL "> failed");               \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_LOAN_V2_RUN_ENSUREPOD)
#undef IBMW_LOAN_V2_RUN_ENSUREPOD

  running_ = true;
  thread_ = std::thread([this]() { PublishLoop(); });
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void PublisherModule::PublishLoop() {
  using clock = std::chrono::steady_clock;

  const auto period_ns = std::chrono::nanoseconds(
      static_cast<int64_t>(1'000'000'000ULL / std::max<uint32_t>(opts_.hz, 1)));
  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);

  uint32_t seq = 0;
  uint64_t loan_failed = 0;
  auto next_tick = clock::now();

  // Pre-compute the msg_type string per size so the hot path does no
  // allocation. Static so each is constructed exactly once, first call.
#define IBMW_LOAN_V2_LOOP_STATIC_TYPE(LBL, N, ALIAS)                           \
  static const std::string msg_type_##N = MakeDdsMsgType<ALIAS>();
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_LOAN_V2_LOOP_STATIC_TYPE)
#undef IBMW_LOAN_V2_LOOP_STATIC_TYPE

  while (running_ && clock::now() < deadline) {
    // Static fan-out by payload_bytes -- identical shape to iox_send so
    // any latency delta is attributable solely to the publish-side path
    // (LoanedSample<T>::Borrow + Publish here vs. heap+Send there).
#define IBMW_LOAN_V2_LOOP_DISPATCH(LBL, N, ALIAS)                              \
  if (opts_.payload_bytes == (N)) {                                            \
    auto sample = ibmw::transport::LoanedSample<ALIAS>::Borrow(                \
        sender_##N##_, msg_type_##N);                                          \
    if (!sample.is_valid()) {                                                  \
      ++loan_failed;                                                           \
    } else {                                                                   \
      /* Direct write into the iceoryx SHM chunk via typed wrapper.    */      \
      /* Same fill helper iox_send uses on its heap object, so the     */      \
      /* wire bytes are byte-identical and the subscriber-side raw     */      \
      /* POD parse (reading BenchPlainHeader at offset 0) works as-is. */      \
      FillBenchPlainPayload(*sample, seq);                                     \
      ibmw::transport::LoanedSample<ALIAS>::Publish(                           \
          sender_##N##_, std::move(sample));                                   \
    }                                                                          \
  } else
    IBMW_BENCH_PLAIN_FOR_EACH(IBMW_LOAN_V2_LOOP_DISPATCH)
    {
      // Caught by Setup() validation; defensive only.
    }
#undef IBMW_LOAN_V2_LOOP_DISPATCH
    ++seq;

    next_tick += period_ns;
    auto now = clock::now();
    if (next_tick > now) {
      std::this_thread::sleep_until(next_tick);
    } else {
      // Behind schedule -- drop the catch-up to avoid burst artifacts
      // skewing the subscriber's percentile tail.
      next_tick = now;
    }
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={} loan_failed={}",
         seq, loan_failed);
}

}  // namespace ibmw::samples::transport_benchmark::iox_loan_v2
