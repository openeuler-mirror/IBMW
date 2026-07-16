// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loan_v2 subscriber module: receives via the high-level
// LoanedSubscriber<T>::AttachBytes() fast lane (Phase 7 §7.3 §U).
// Functionally byte-equivalent to the previous raw-POD callback path
// (RegisterRawPodSubscriber): the callback gets a const reference to
// the SHM chunk plus its byte size, with no MessageFrame / no
// deserialization / no filter chain. The dispatcher routes the
// AttachLoaned op to iceoryx, which still owns the listener thread
// and the chunk lifetime; iox_loan_v2 just no longer pokes the
// extension API directly.
//
// We register one subscription per Plain bench type (5 URLs total)
// so the publisher's payload_bytes yaml selection picks the matching
// URL transparently. The handler reads BenchPlainHeader from offset 0
// regardless of which N is live.

#pragma once

#include <atomic>
#include <mutex>

#include "cpp/module_base.h"

#include "base/benchmark_util.h"
#include "bench_options.h"
#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::iox_loan_v2 {

class SubscriberModule : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanV2SubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::Runtime core_;
  BenchOptions opts_;

  // The iceoryx Listener invokes the callback from its internal thread
  // pool. stats_mu_ guards concurrent Add() calls -- only one size is
  // live per run today, but the 5-URL registration leaves room for
  // parallel producers in future experiments.
  std::mutex stats_mu_;
  ibmw::common::util::LatencyStats stats_;

  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> dropped_{0};      // size mismatch / short chunk
  std::atomic<bool> finalized_{false};
};

}  // namespace ibmw::samples::transport_benchmark::iox_loan_v2
