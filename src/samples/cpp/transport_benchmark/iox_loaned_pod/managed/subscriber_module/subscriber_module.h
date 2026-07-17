// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loaned_pod subscriber module: registers a raw POD callback on the
// iceoryx driver. Each call computes latency = MonotonicNs() - send_ns
// and feeds it into a LatencyStats; on Stop() the summary is written to
// stdout and (if csv_path is set) appended to a CSV file shared with
// the other five benchmark samples.

#pragma once

#include <atomic>
#include <mutex>

#include "cpp/module_base.h"

#include "base/benchmark_util.h"
#include "bench_options.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::iox_loaned_pod {

class SubscriberModule : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanedPodSubscriberModule"};
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

  // The iceoryx Listener invokes the callback from its internal thread
  // pool. We add() into LatencyStats from there; stats_mu_ guards
  // concurrent Add() calls (single subscriber today, but cheap insurance).
  std::mutex stats_mu_;
  ibmw::common::util::LatencyStats stats_;

  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> dropped_{0};      // parse failures
  std::atomic<bool> finalized_{false};
};

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_pod
