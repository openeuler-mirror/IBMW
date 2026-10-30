// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <chrono>

#include "bench_payload.h"

namespace ibmw::samples::transport_benchmark::shm_loaned_pod {

namespace {
// Topic + msg_type strings. They MUST match the subscriber side
// byte-for-byte; the shm topic url is derived from
// (topic, msg_type) as "/transport_zc_pod/<topic>/<msg_type>".
constexpr std::string_view kTopic = "iox_bench_pod_topic";
constexpr std::string_view kMsgType = "BenchPod";
}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  // The shm driver is injected between Initialize() and Start(), so it is
  // NOT available here: only opts loading lives in Setup; the POD publisher
  // is created by RegisterWithDriver().
  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  if (opts_.payload_bytes < sizeof(BenchPodHeader)) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: payload_bytes={} < sizeof(BenchPodHeader)={}",
           opts_.payload_bytes, sizeof(BenchPodHeader));
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup OK: label={} payload_bytes={} hz={} "
         "duration_sec={} warmup={}",
         opts_.label, opts_.payload_bytes, opts_.hz, opts_.duration_sec,
         opts_.warmup_messages);
  return true;
}

bool PublisherModule::RegisterWithDriver() {
  if (driver_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: shm driver not injected");
    return false;
  }

  // Hint the shm segment about the largest chunk we'll need;
  // the driver still auto-grows if exceeded, but supplying the right hint
  // up-front avoids cold-path reallocations during the warmup run.
  raw_pub_ = ibmw::extensions::shm_extension::CreateShmRawPodPublisher(
      driver_, kTopic, kMsgType, opts_.payload_bytes);
  if (raw_pub_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: CreateShmRawPodPublisher failed");
    return false;
  }
  return true;
}

bool PublisherModule::Run() {
  if (raw_pub_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: RegisterWithDriver() did not succeed before Start()");
    return false;
  }

  running_ = true;
  thread_ = std::thread([this]() { PublishLoop(); });
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  raw_pub_.reset();
}

void PublisherModule::PublishLoop() {
  using clock = std::chrono::steady_clock;

  const auto period_ns = std::chrono::nanoseconds(
      static_cast<int64_t>(1'000'000'000ULL / std::max<uint32_t>(opts_.hz, 1)));
  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);

  uint32_t seq = 0;
  uint64_t loan_failed = 0;
  auto next_tick = clock::now();

  while (running_ && clock::now() < deadline) {
    auto loaned = raw_pub_->Borrow(opts_.payload_bytes);
    if (!loaned.valid()) {
      ++loan_failed;
    } else {
      // Direct write into the shm chunk -- this is the whole
      // point of the zero-copy POD path. send_ns is captured INSIDE
      // FillBenchPod via MonotonicNs() to minimize the gap between
      // timestamping and the publish() syscall.
      FillBenchPod(loaned.data(), opts_.payload_bytes, seq);
      raw_pub_->Publish(std::move(loaned));
    }
    ++seq;

    // Fixed-rate pacing: sleep until the next deadline rather than
    // sleep_for(period) so jitter does not accumulate over the run.
    next_tick += period_ns;
    auto now = clock::now();
    if (next_tick > now) {
      std::this_thread::sleep_until(next_tick);
    } else {
      // We're behind schedule; reset the cadence to "now" so we don't
      // try to catch up by spamming. This avoids burst-publish artifacts
      // skewing the subscriber's percentile tail.
      next_tick = now;
    }
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={} loan_failed={}",
         seq, loan_failed);
}

}  // namespace ibmw::samples::transport_benchmark::shm_loaned_pod
