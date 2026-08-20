// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>

#include "benchmark_run_state.h"

namespace ibmw::samples::transport_benchmark::iox_send {

namespace {
// Distinct from iox_loaned_ctx's "iox_bench_ctx_topic" so the two
// samples can run side-by-side on the same iceoryx RouDi without their
// service URLs colliding. Each Plain bench type still gets its own
// (topic, msg_type) entry so subscribers can route by type.
constexpr std::string_view kDefaultTopic = "iox_bench_send_topic";
}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  const std::string topic = opts_.topic.empty() ? std::string(kDefaultTopic) : opts_.topic;
  if (PayloadSizeLabel(opts_.payload_bytes) == nullptr) {
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
  // nothing on the iceoryx side beyond a registry entry. The shared
  // GetPublisher(kTopic) call de-dups on topic name and yields the
  // SenderRef that each typed SenderAccess<T> wraps; the explicit
  // .RegisterSendType() then registers the type's iceoryx publish URL
  // keyed on (topic, msg_type). SenderAccess<T> is immobile, so each
  // optional slot is emplaced directly from the SenderRef rather than
  // via the publisher_for<T> factory (which returns by value).
#define IBMW_PUB_SETUP_REGISTER(LBL, N, ALIAS)                                \
  {                                                                           \
    sender_##N##_.emplace(core_.GetTransport().GetPublisher(topic));         \
    if (!sender_##N##_->RegisterSendType()) {                                 \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,           \
             "PublisherModule: RegisterSendType<" LBL "> failed");            \
      return false;                                                           \
    }                                                                         \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_SETUP_REGISTER)
#undef IBMW_PUB_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} message_count={} start_delay_ms={} warmup={}",
         opts_.label, opts_.payload_bytes,
         PayloadSizeLabel(opts_.payload_bytes), opts_.hz, opts_.duration_sec,
         opts_.message_count, opts_.start_delay_ms,
         opts_.warmup_messages);
  return true;
}

bool PublisherModule::Run() {
  // No driver-bound resources -- iox_send only uses standard
  // ibmw::transport::Send, which routes through the engine's bus and
  // hands off to the iceoryx driver via the same MessageHandler path
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

  if (opts_.start_delay_ms != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.start_delay_ms));
  }

  const auto period_ns = std::chrono::nanoseconds(
      static_cast<int64_t>(1'000'000'000ULL / std::max<uint32_t>(opts_.hz, 1)));
  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);

  uint32_t seq = 0;
  auto next_tick = clock::now();

  const uint64_t fixed_total = static_cast<uint64_t>(opts_.warmup_messages) +
                               static_cast<uint64_t>(opts_.message_count);
  while (running_ &&
         (opts_.message_count != 0 ? seq < fixed_total
                                   : clock::now() < deadline)) {
    // Static fan-out by payload_bytes -- mirrors iox_loaned_ctx so
    // any latency delta vs. that sample is attributable solely to the
    // publish-side path (heap+memcpy here vs. ctx-meta loan there).
#define IBMW_PUB_LOOP_DISPATCH(LBL, N, ALIAS)                                  \
  if (opts_.payload_bytes == (N)) {                                            \
    /* Heap-allocate the message: 4 MB on the stack would blow past   */       \
    /* the default 8 MB pthread stack on the publisher thread.        */       \
    auto msg = std::make_unique<ALIAS>();                                      \
    FillBenchPlainPayload(*msg, seq);                                          \
    /* Standard send path: engine CDR-serializes into a heap         */        \
    /* BufferArray, then the iceoryx driver borrows an SHM chunk    */        \
    /* and memcpy's the buffer into it. Wire format (pkg_size +     */        \
    /* sertype + KV header + CDR payload) matches iox_loaned_ctx     */        \
    /* exactly so subscribers are byte-interchangeable.              */        \
    sender_##N##_->Send(*msg);                                                 \
  } else
    IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_LOOP_DISPATCH)
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

  const auto status_value = MakePublisherRunStatus(
      opts_.message_count, opts_.warmup_messages, seq,
      clock::now() >= deadline);
  if (!opts_.publisher_status_path.empty()) {
    const std::string temporary_path = opts_.publisher_status_path + ".tmp";
    bool written = false;
    {
      std::ofstream status(temporary_path, std::ios::trunc);
      if (status) {
        status << "{\n"
               << "  \"completion\": " << (status_value.completion ? "true" : "false") << ",\n"
               << "  \"attempted_total\": " << status_value.attempted_total << ",\n"
               << "  \"attempted_measured\": " << status_value.attempted_measured << ",\n"
               << "  \"valid\": " << (status_value.valid ? "true" : "false") << "\n"
               << "}\n";
        status.flush();
        written = status.good();
      }
    }
    if (!written || std::rename(temporary_path.c_str(),
                                opts_.publisher_status_path.c_str()) != 0) {
      std::remove(temporary_path.c_str());
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
             "PublisherModule: cannot atomically write publisher_status_path={}",
             opts_.publisher_status_path);
    }
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={}", seq);
}

}  // namespace ibmw::samples::transport_benchmark::iox_send
