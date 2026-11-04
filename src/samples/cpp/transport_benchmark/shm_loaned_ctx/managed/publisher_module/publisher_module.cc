// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

#include "pacing_schedule.h"
#include "type_support/dds/dds_transport.h"

namespace ibmw::samples::transport_benchmark::shm_loaned_ctx {

namespace {

// The subscriber (iox_send's module) reads this topic from its yaml
// (bench_options.topic).
constexpr std::string_view kTopic = "shm_bench_ctx_topic";

// Must match what RegisterSendType<T>() registered ("dds:" + type name).
template <typename T>
std::string MakeDdsMsgType() {
  return std::string("dds:") + ibmw::dds::DdsTypeName<T>::get();
}

using iox_loaned_ctx::AdvancePacingDeadline;
using iox_loaned_ctx::BatchPacingPeriod;

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

  // RegisterSendType creates the driver-side publisher the ctx-meta loan
  // borrows from; it is only accepted while the engine is in Init.
#define IBMW_SHM_CTX_SETUP_REGISTER(LBL, N, ALIAS)                             \
  {                                                                            \
    auto& slot = slot_##N##_;                                                  \
    slot.sender = core_.GetTransport().GetPublisher(std::string(kTopic));      \
    if (!ibmw::transport::RegisterSendType<ALIAS>(slot.sender)) {              \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: RegisterSendType<" LBL "> failed");             \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_CTX_SETUP_REGISTER)
#undef IBMW_SHM_CTX_SETUP_REGISTER

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
           "PublisherModule: transport driver not injected before Run()");
    return false;
  }

#define IBMW_SHM_CTX_RUN_CREATE(LBL, N, ALIAS)                                 \
  slot_##N##_.typed_pub =                                                      \
      ibmw::extensions::shm_extension::CreateShmCtxMetaTypedPublisher<ALIAS>(  \
          driver_, kTopic, MakeDdsMsgType<ALIAS>());
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_CTX_RUN_CREATE)
#undef IBMW_SHM_CTX_RUN_CREATE

  running_ = true;
  thread_ = std::thread([this]() { PublishLoop(); });
  if (opts_.measure_begin_monotonic_ns != 0) {
    std::cout << "PERF_PUBLISHER_RUNNING" << std::endl;
  }
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
#define IBMW_SHM_CTX_STOP_RESET(LBL, N, ALIAS) slot_##N##_.typed_pub.reset();
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_CTX_STOP_RESET)
#undef IBMW_SHM_CTX_STOP_RESET
}

void PublisherModule::PublishLoop() {
  using clock = std::chrono::steady_clock;

  const uint32_t batch = std::max<uint32_t>(opts_.batch_size, 1);
  const auto batch_period_ns = BatchPacingPeriod(opts_.hz, batch);
  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);
  // Shared measurement window of the shm core_metrics gate (same arithmetic as
  // iox_loaned_ctx); without it the duration-based loop below is unchanged.
  const bool shared_window = opts_.measure_begin_monotonic_ns != 0;
  const auto begin = clock::time_point(std::chrono::nanoseconds(opts_.measure_begin_monotonic_ns));
  const auto end = clock::time_point(std::chrono::nanoseconds(opts_.measure_end_monotonic_ns));
  const auto send_begin = begin - std::chrono::seconds(opts_.warmup_sec);
  const auto send_end = end + std::chrono::seconds(opts_.duration_sec - opts_.warmup_sec - opts_.measured_sec);
  const auto stop_at = shared_window ? send_end : deadline;

  // Same header as Send(): the subscriber sees byte-identical frames.
  ibmw::transport::Context ctx;
  ctx.SetSerializationType("cdr");

  uint32_t seq = 0;
  uint64_t publish_failed = 0;
  auto next_tick = shared_window ? send_begin : clock::now();
  if (shared_window && clock::now() < send_begin) {
    std::this_thread::sleep_until(send_begin);
  }

  while (running_ && clock::now() < stop_at) {
    for (uint32_t batch_pos = 0; batch_pos < batch && running_ && clock::now() < stop_at; ++batch_pos) {
#define IBMW_SHM_CTX_LOOP_DISPATCH(LBL, N, ALIAS)                              \
  if (opts_.payload_bytes == (N)) {                                            \
    /* Heap-allocated as in iox_loaned_ctx: 4 MB would not fit the stack. */   \
    auto msg = std::make_unique<ALIAS>();                                      \
    FillBenchPlainPayload(*msg, seq);                                          \
    /* Window bookkeeping only on the gate path; the matrix path publishes   \
       exactly as before. */                                                   \
    if (shared_window) {                                                       \
      const uint64_t send_ns = ExtractBenchPlainSendNs(*msg);                  \
      const bool measured = send_ns >= opts_.measure_begin_monotonic_ns && send_ns < opts_.measure_end_monotonic_ns; \
      if (measured) { if (!have_attempt_) first_attempt_seq_ = seq; last_attempt_seq_ = seq; have_attempt_ = true; ++measured_attempts_; } \
      if (!slot_##N##_.typed_pub->Publish(ctx, *msg)) {                        \
        ++publish_failed;                                                      \
        if (measured) ++measured_fail_;                                        \
      } else if (measured) {                                                   \
        if (!have_success_) first_success_seq_ = seq; last_success_seq_ = seq; have_success_ = true; ++measured_success_; \
      }                                                                        \
    } else if (!slot_##N##_.typed_pub->Publish(ctx, *msg)) {                   \
      ++publish_failed;                                                        \
    }                                                                          \
  } else
      IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_CTX_LOOP_DISPATCH)
      {
        ++publish_failed;  // caught by Setup() validation; defensive only
      }
#undef IBMW_SHM_CTX_LOOP_DISPATCH
      ++seq;
    }
    if (opts_.hz != 0) {
      next_tick += batch_period_ns;
      next_tick = AdvancePacingDeadline<clock>(next_tick, batch_period_ns, clock::now());
      std::this_thread::sleep_until(next_tick);
    }
  }
  completed_send_window_ = clock::now() >= stop_at;

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={} publish_failed={}",
         seq, publish_failed);
}

}  // namespace ibmw::samples::transport_benchmark::shm_loaned_ctx
