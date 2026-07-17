// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// dds_send_tcp subscriber module: standard IBMW receive-side stack -- no
// extension-only API. Registers OnMessage<PlainT> for each Plain bench
// type in the bank; whichever one the publisher actually fires (per
// payload_bytes in publisher.yaml) feeds the same LatencyStats. On
// Stop(), the summary is logged and (if csv_path is set) appended to
// the shared CSV file.
//
// Design IDENTICAL to dds_loaned subscriber_module so that any
// latency delta between the two samples is attributable purely to the
// publish-side path (Send + CDR encode + transport copy here vs.
// LoanedMessage zero-copy there). The only structural difference is
// the absence of the dds driver pointer -- dds_send_tcp never calls an
// extension-specific API.

#pragma once

#include <atomic>
#include <mutex>

#include "cpp/module_base.h"

#include "base/benchmark_util.h"
#include "bench_options.h"

namespace ibmw::samples::transport_benchmark::dds_send_tcp {

class SubscriberModule : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "DdsSendSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::Runtime core_;
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

}  // namespace ibmw::samples::transport_benchmark::dds_send_tcp
