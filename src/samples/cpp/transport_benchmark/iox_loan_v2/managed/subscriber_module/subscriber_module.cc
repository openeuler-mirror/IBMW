// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include <cstring>
#include <string>

#include "cpp/transport/loaned_subscriber.h"
#include "type_support/dds/dds_transport.h"

namespace ibmw::samples::transport_benchmark::iox_loan_v2 {

namespace {
// MUST exactly match publisher_module.cc.
constexpr std::string_view kTopic = "iox_bench_loan_v2_topic";

template <typename T>
std::string MakeDdsMsgType() {
  return std::string("dds:") + ibmw::dds::DdsTypeName<T>::get();
}
}  // namespace

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  stats_ = ibmw::common::util::LatencyStats(opts_.label);
  // Reserve room for `duration_sec * hz` plus 20% headroom so Add() is
  // amortised O(1) without vector reallocation skewing tail latency.
  size_t expected = static_cast<size_t>(opts_.duration_sec) *
                    static_cast<size_t>(opts_.hz);
  stats_.Reserve(expected + expected / 5 + 64);

  // Resolve the subscriber once. The transport bus de-dups on topic
  // name, so every per-type AttachBytes() below shares the same
  // ReceiverRef and the dispatcher fans out to per-(topic, msg_type)
  // iceoryx URLs internally. AttachLoaned() must be wired in Setup()
  // because the transport bus rejects new attach calls after start.
  auto subscriber = core_.GetTransport().GetSubscriber(std::string(kTopic));

  // Per-chunk handler. The payload starts with a BenchPlainHeader
  // (24 B) regardless of N, so we can pull send_ns out of any size as
  // long as size >= sizeof(BenchPlainHeader). We treat the typed
  // `const T&` as the SHM chunk base and reinterpret the first 24 B as
  // a BenchPlainHeader, mirroring the previous raw-callback shape.
  auto handle_chunk = [this](const void* payload, size_t size) {
    uint64_t recv_ns = ibmw::common::util::MonotonicNs();

    if (payload == nullptr || size < sizeof(BenchPlainHeader)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    BenchPlainHeader header;
    // Copy out via memcpy to avoid strict-aliasing / unaligned-read UB
    // even though BenchPlainHeader is trivially copyable and the
    // iceoryx chunk is 8-byte aligned in practice.
    std::memcpy(&header, payload, sizeof(BenchPlainHeader));

    uint64_t latency_ns =
        (recv_ns >= header.send_ns) ? (recv_ns - header.send_ns) : 0;
    uint32_t seq = static_cast<uint32_t>(header.sequence_num);

    // Skip warmup samples to avoid first-publish JIT/COW penalties
    // skewing the percentile tail.
    if (seq < opts_.warmup_messages) return;

    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      stats_.Add(latency_ns);
    }
    received_.fetch_add(1, std::memory_order_relaxed);
  };

  // Attach one zero-copy receive subscription per Plain bench type.
  // The dispatcher routes AttachLoaned to iceoryx (the only driver
  // overriding it today), which composes the same
  // "/transport_zc_pod/<topic>/<msg_type>" URL the publisher used.
  // The typed `const ALIAS&` is reinterpreted by `handle_chunk` as a
  // raw byte region; we pass `size` through so flexible-suffix sizes
  // are handled correctly even though sizeof(ALIAS) is fixed.
#define IBMW_LOAN_V2_SUB_ATTACH(LBL, N, ALIAS)                                 \
  {                                                                            \
    const std::string msg_type = MakeDdsMsgType<ALIAS>();                      \
    bool ok = ibmw::transport::LoanedSubscriber<ALIAS>::AttachBytes(           \
        subscriber, msg_type,                                                  \
        [handle_chunk](const ALIAS& msg, size_t size) {                        \
          handle_chunk(static_cast<const void*>(&msg), size);                  \
        });                                                                    \
    if (!ok) {                                                                 \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "SubscriberModule: LoanedSubscriber<" LBL ">::AttachBytes "       \
             "failed");                                                        \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_LOAN_V2_SUB_ATTACH)
#undef IBMW_LOAN_V2_SUB_ATTACH

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Setup OK: label={} payload_bytes={} hz={} "
         "duration_sec={} csv={}",
         opts_.label, opts_.payload_bytes, opts_.hz, opts_.duration_sec,
         opts_.csv_path.empty() ? "<none>" : opts_.csv_path);
  return true;
}

bool SubscriberModule::Run() {
  // Nothing to do at Run(): deliveries already flow through the
  // LoanedSubscriber<T>::AttachBytes() handlers installed in Setup().
  // No driver pointer needed -- iox_loan_v2 no longer touches the
  // extension API directly.
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

}  // namespace ibmw::samples::transport_benchmark::iox_loan_v2
