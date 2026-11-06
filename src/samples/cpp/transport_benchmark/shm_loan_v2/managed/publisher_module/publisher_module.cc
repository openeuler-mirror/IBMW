// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>
#include <string>

#include "type_support/dds/dds_transport.h"

namespace ibmw::samples::transport_benchmark::shm_loan_v2 {

namespace {

// Same topic as iox_loan_v2: the subscriber is iox_loan_v2's, unchanged.
constexpr std::string_view kTopic = "iox_bench_loan_v2_topic";

// Must match what RegisterSendType<T>() registered ("dds:" + type name).
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

  // Setup() runs while the dispatcher is in Init, which is when both the
  // send type and the loaned POD topic must be registered.
#define IBMW_SHM_LOAN_V2_SETUP_REGISTER(LBL, N, ALIAS)                         \
  {                                                                            \
    sender_##N##_ = core_.GetTransport().GetPublisher(std::string(kTopic));    \
    if (!ibmw::transport::RegisterSendType<ALIAS>(sender_##N##_)) {            \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: RegisterSendType<" LBL "> failed");             \
      return false;                                                            \
    }                                                                          \
    if (!sender_##N##_.RegisterLoanedPodTopic(MakeDdsMsgType<ALIAS>(), (N),    \
                                              alignof(ALIAS))) {               \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: RegisterLoanedPodTopic<" LBL "> failed");       \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_LOAN_V2_SETUP_REGISTER)
#undef IBMW_SHM_LOAN_V2_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} warmup={}",
         opts_.label, opts_.payload_bytes,
         PayloadSizeLabel(opts_.payload_bytes), opts_.hz, opts_.duration_sec,
         opts_.warmup_messages);
  return true;
}

bool PublisherModule::Run() {
  running_ = true;
  thread_ = std::thread([this]() { PublishLoop(); });
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

// Identical to iox_loan_v2's loop, so latency deltas come from the transport.
void PublisherModule::PublishLoop() {
  using clock = std::chrono::steady_clock;

  const auto period_ns = std::chrono::nanoseconds(
      static_cast<int64_t>(1'000'000'000ULL / std::max<uint32_t>(opts_.hz, 1)));
  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);

  uint32_t seq = 0;
  uint64_t loan_failed = 0;
  auto next_tick = clock::now();

#define IBMW_SHM_LOAN_V2_LOOP_STATIC_TYPE(LBL, N, ALIAS)                       \
  static const std::string msg_type_##N = MakeDdsMsgType<ALIAS>();
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_LOAN_V2_LOOP_STATIC_TYPE)
#undef IBMW_SHM_LOAN_V2_LOOP_STATIC_TYPE

  while (running_ && clock::now() < deadline) {
#define IBMW_SHM_LOAN_V2_LOOP_DISPATCH(LBL, N, ALIAS)                          \
  if (opts_.payload_bytes == (N)) {                                            \
    auto sample = ibmw::transport::LoanedSample<ALIAS>::Borrow(                \
        sender_##N##_, msg_type_##N);                                          \
    if (!sample.is_valid()) {                                                  \
      ++loan_failed;                                                           \
    } else {                                                                   \
      FillBenchPlainPayload(*sample, seq);                                     \
      ibmw::transport::LoanedSample<ALIAS>::Publish(                           \
          sender_##N##_, std::move(sample));                                   \
    }                                                                          \
  } else
    IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_LOAN_V2_LOOP_DISPATCH)
    {
      // Caught by Setup() validation; defensive only.
    }
#undef IBMW_SHM_LOAN_V2_LOOP_DISPATCH
    ++seq;

    next_tick += period_ns;
    auto now = clock::now();
    if (next_tick > now) {
      std::this_thread::sleep_until(next_tick);
    } else {
      next_tick = now;
    }
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={} loan_failed={}",
         seq, loan_failed);
}

}  // namespace ibmw::samples::transport_benchmark::shm_loan_v2
