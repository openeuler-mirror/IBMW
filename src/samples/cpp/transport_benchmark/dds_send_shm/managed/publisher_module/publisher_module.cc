// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>

namespace ibmw::samples::transport_benchmark::dds_send_shm {

namespace {
// Distinct from dds_loaned's "dds_bench_loaned_topic" so the two
// samples can run side-by-side on the same host without their RTPS
// discovery / topic registries colliding (publisher.yaml also bumps
// domain_id 42 -> 43 to belt-and-braces this). Each non-Plain bench
// type still gets its own (topic, msg_type) entry so subscribers can
// route by type.
constexpr std::string_view kTopic = "dds_bench_send_shm_topic";

// Pull the dds_send_shm-specific bank helpers into the module namespace so
// the existing X-macro expansions stay readable without explicit
// qualification.
using dds_send_bank::SendPayloadSizeLabel;
using dds_send_bank::FillBenchSendPayload;

bool IsPublisherProfileEnabled() noexcept {
  const char* value = std::getenv("IBMW_DDS_SEND_SHM_PUBLISHER_PROFILE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}
}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  if (SendPayloadSizeLabel(opts_.payload_bytes) == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: payload_bytes={} not in supported bench "
           "matrix (1024/4096/16384/65536/262144/1048576/4194304)",
           opts_.payload_bytes);
    return false;
  }

  // Claim ALL 5 typed (topic, msg_type) slots in Setup() -- the
  // transport bus rejects new GetPublisher/RegisterSendType calls
  // "after start", and we cannot prove at Setup time which one a
  // future yaml will pick. Idle publisher URLs cost essentially
  // nothing on the dds side beyond a registry entry. The shared
  // GetPublisher(kTopic) returns the same SenderRef each call (the
  // transport bus de-dups on topic name); each typed RegisterSendType
  // adds its own DDS DataWriter keyed on (topic, msg_type).
#define IBMW_PUB_SETUP_REGISTER(LBL, N, ALIAS)                                \
  {                                                                           \
    sender_##N##_ = core_.GetTransport().GetPublisher(std::string(kTopic));   \
    if (!ibmw::transport::RegisterSendType<ALIAS>(sender_##N##_)) {           \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,           \
             "PublisherModule: RegisterSendType<" LBL "> failed");            \
      return false;                                                           \
    }                                                                         \
  }
  IBMW_BENCH_SEND_FOR_EACH(IBMW_PUB_SETUP_REGISTER)
#undef IBMW_PUB_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} message_count={} start_delay_ms={} warmup={}",
         opts_.label, opts_.payload_bytes,
         SendPayloadSizeLabel(opts_.payload_bytes), opts_.hz, opts_.duration_sec,
         opts_.message_count, opts_.start_delay_ms, opts_.warmup_messages);
  return true;
}

bool PublisherModule::Run() {
  // No driver-bound resources -- dds_send_shm only uses standard
  // ibmw::transport::Send, which routes through the engine's bus and
  // hands off to the dds driver via the same MessageHandler path
  // as any other transport. Publisher thread can spin up immediately.
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
  const bool profile_enabled = IsPublisherProfileEnabled();
  uint64_t fill_count = 0;
  uint64_t fill_total_ns = 0;
  uint64_t fill_max_ns = 0;
  uint64_t send_count = 0;
  uint64_t send_total_ns = 0;
  uint64_t send_max_ns = 0;
  if (opts_.start_delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.start_delay_ms));
  }
  auto next_tick = clock::now();

  auto should_continue = [&]() {
    if (!running_) return false;
    if (opts_.message_count > 0) return seq < opts_.message_count;
    return clock::now() < deadline;
  };

  while (should_continue()) {
    // Static fan-out by payload_bytes -- mirrors dds_loaned so
    // any latency delta vs. that sample is attributable solely to the
    // publish-side path (CDR encode + transport copy here vs.
    // LoanedMessage zero-copy there).
#define IBMW_PUB_LOOP_DISPATCH(LBL, N, ALIAS)                                  \
  if (opts_.payload_bytes == (N)) {                                            \
    /* Heap-allocate the message: 4 MB on the stack would blow past   */       \
    /* the default 8 MB pthread stack on the publisher thread.        */       \
    auto msg = std::make_unique<ALIAS>();                                      \
    const uint64_t fill_start_ns =                                             \
        profile_enabled ? ibmw::common::util::MonotonicNs() : 0;               \
    FillBenchSendPayload(*msg, seq);                                           \
    if (profile_enabled) {                                                     \
      const uint64_t fill_ns =                                                 \
          ibmw::common::util::MonotonicNs() - fill_start_ns;                   \
      ++fill_count;                                                            \
      fill_total_ns += fill_ns;                                                \
      fill_max_ns = std::max(fill_max_ns, fill_ns);                            \
    }                                                                          \
    /* Standard send path: engine CDR-serializes into a heap         */        \
    /* BufferArray, then the dds driver hands the buffer to FastDDS  */        \
    /* via GenericDdsPubSubType<T>::serialize. Wire format (CDR)     */        \
    /* matches dds_loaned exactly so subscribers are byte-           */        \
    /* interchangeable.                                              */        \
    const uint64_t send_start_ns =                                             \
        profile_enabled ? ibmw::common::util::MonotonicNs() : 0;               \
    ibmw::transport::Send(sender_##N##_, *msg);                                \
    if (profile_enabled) {                                                     \
      const uint64_t send_ns =                                                 \
          ibmw::common::util::MonotonicNs() - send_start_ns;                   \
      ++send_count;                                                            \
      send_total_ns += send_ns;                                                \
      send_max_ns = std::max(send_max_ns, send_ns);                            \
    }                                                                          \
  } else
    IBMW_BENCH_SEND_FOR_EACH(IBMW_PUB_LOOP_DISPATCH)
    {
      // Caught by the Setup() validation; defensive only.
    }
#undef IBMW_PUB_LOOP_DISPATCH
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
         "PublisherModule done: total_seq={} mode={} target_count={} duration_sec={}",
         seq, opts_.message_count > 0 ? "fixed_count" : "duration",
         opts_.message_count, opts_.duration_sec);
  if (profile_enabled) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
           "PublisherModule publish profile: fill_count={} fill_avg_ns={} "
           "fill_max_ns={} send_count={} send_avg_ns={} send_max_ns={}",
           fill_count, fill_count == 0 ? 0 : fill_total_ns / fill_count,
           fill_max_ns, send_count, send_count == 0 ? 0 : send_total_ns / send_count,
           send_max_ns);
  }
}

}  // namespace ibmw::samples::transport_benchmark::dds_send_shm
