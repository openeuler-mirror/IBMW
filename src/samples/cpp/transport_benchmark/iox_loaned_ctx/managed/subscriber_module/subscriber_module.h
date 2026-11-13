// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loaned_ctx subscriber module: standard IBMW receive-side stack
// (no extension-only API). Registers an OnMessage<PlainT> callback for
// each Plain bench type in the bank; whichever one the publisher
// actually fires (per payload_bytes in publisher.yaml) feeds the same
// LatencyStats. On Stop(), the summary is written to stdout and (if
// csv_path is set) appended to the shared CSV file.
//
// Why register all 5 callbacks even though only one will fire?
//   The transport bus rejects new GetSubscriber/OnMessage calls "after
//   start", so we cannot lazily wire the type the publisher chooses --
//   we have to claim every iceoryx receive URL up-front. Idle URLs cost
//   nothing on the iceoryx side beyond a registry entry.
//
// We still inject the driver pointer so we can confirm the transport selected
// at runtime and emit a clear log line for diagnostic parity with the publisher
// side, but we never call any extension-specific APIs from the receive path.
// The module has no iceoryx dependency: the shm core_metrics gate compiles it
// into shm_loaned_ctx_lossless_subscriber_app.

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

#include "cpp/module_base.h"

#include "base/benchmark_util.h"
#include "bench_options.h"

namespace ibmw::runtime::core::transport {
class TransportDriver;
}  // namespace ibmw::runtime::core::transport

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {

class SubscriberModule : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanedCtxSubscriberModule"};
  }

  void SetDriver(ibmw::runtime::core::transport::TransportDriver* d) { driver_ = d; }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::Runtime core_;
  ibmw::runtime::core::transport::TransportDriver* driver_{nullptr};
  BenchOptions opts_;

  // The DDS-style subscription executor delivers callbacks from the
  // engine thread pool. Single-active-type per run, but stats_mu_ is
  // cheap insurance against future fan-in (and against any debug build
  // that fires multiple typed handlers concurrently).
  std::mutex stats_mu_;
  ibmw::common::util::LatencyStats stats_;

  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> dropped_{0};
  uint64_t unique_{0};
  uint64_t duplicate_{0};
  uint64_t out_of_order_{0};
  uint64_t missing_{0};
  uint32_t first_sequence_{0};
  uint32_t last_sequence_{0};
  bool have_last_sequence_{false};
  std::atomic<bool> finalized_{false};
  std::chrono::steady_clock::time_point measure_begin_{};
  std::chrono::steady_clock::time_point measure_end_{};
};

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
