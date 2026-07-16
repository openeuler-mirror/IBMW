// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>
#include <memory>

namespace ibmw::samples::transport_benchmark::net_send_tcp {

namespace {
// Distinct from dds_loaned's "dds_bench_loaned_topic" so the two
// samples can run side-by-side on the same host without their RTPS
// discovery / topic registries colliding (publisher.yaml also bumps
// domain_id 42 -> 43 to belt-and-braces this). Each non-Plain bench
// type still gets its own (topic, msg_type) entry so subscribers can
// route by type.
constexpr std::string_view kTopic = "net_bench_send_tcp_topic";

// Pull the net_send_tcp-specific bank helpers into the module namespace so
// the existing X-macro expansions stay readable without explicit
// qualification.
using dds_send_bank::SendPayloadSizeLabel;
using dds_send_bank::FillBenchSendPayload;
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
         "duration_sec={} warmup={}",
         opts_.label, opts_.payload_bytes,
         SendPayloadSizeLabel(opts_.payload_bytes), opts_.hz, opts_.duration_sec,
         opts_.warmup_messages);
  return true;
}

bool PublisherModule::Run() {
  // No driver-bound resources -- net_send_tcp only uses standard
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
  auto next_tick = clock::now();

  while (running_ && clock::now() < deadline) {
    // Static fan-out by payload_bytes -- mirrors dds_loaned so
    // any latency delta vs. that sample is attributable solely to the
    // publish-side path (CDR encode + transport copy here vs.
    // LoanedMessage zero-copy there).
#define IBMW_PUB_LOOP_DISPATCH(LBL, N, ALIAS)                                  \
  if (opts_.payload_bytes == (N)) {                                            \
    /* Heap-allocate the message: 4 MB on the stack would blow past   */       \
    /* the default 8 MB pthread stack on the publisher thread.        */       \
    auto msg = std::make_unique<ALIAS>();                                      \
    FillBenchSendPayload(*msg, seq);                                           \
    /* Standard send path: engine CDR-serializes into a heap         */        \
    /* BufferArray, then the dds driver hands the buffer to FastDDS  */        \
    /* via GenericDdsPubSubType<T>::serialize. Wire format (CDR)     */        \
    /* matches dds_loaned exactly so subscribers are byte-           */        \
    /* interchangeable.                                              */        \
    ibmw::transport::Send(sender_##N##_, *msg);                                \
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
         "PublisherModule done: total_seq={}", seq);
}

}  // namespace ibmw::samples::transport_benchmark::net_send_tcp
