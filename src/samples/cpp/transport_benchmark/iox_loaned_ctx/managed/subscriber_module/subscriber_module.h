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
// We still inject the iceoryx driver pointer so we can confirm the
// transport selected at runtime and emit a clear log line for diagnostic
// parity with the publisher side, but we never call any
// extension-specific APIs from the receive path.

#pragma once

#include <atomic>
#include <mutex>

#include "cpp/module_base.h"

#include "base/benchmark_util.h"
#include "bench_options.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {

class SubscriberModule : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanedCtxSubscriberModule"};
  }

  void SetIceoryxDriver(
      ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* d) {
    driver_ = d;
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::Runtime core_;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* driver_{nullptr};
  BenchOptions opts_;

  // The DDS-style subscription executor delivers callbacks from the
  // engine thread pool. Single-active-type per run, but stats_mu_ is
  // cheap insurance against future fan-in (and against any debug build
  // that fires multiple typed handlers concurrently).
  std::mutex stats_mu_;
  ibmw::common::util::LatencyStats stats_;

  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<bool> finalized_{false};
};

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
