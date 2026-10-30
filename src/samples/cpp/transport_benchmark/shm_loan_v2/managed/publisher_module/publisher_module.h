// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loan_v2 publisher module: iox_loan_v2 on the self-developed shm
// transport, using only the public IBMW API.
//
// The per-tick path is the same as iox_loan_v2 (LoanedSample<T>::Borrow,
// fill in place, ::Publish). The one-time POD topic registration differs:
// iox_loan_v2 casts the engine's driver to IceoryxTransportDriver and calls
// EnsurePodTopic(); here SenderRef::RegisterLoanedPodTopic() goes through the
// dispatcher to whichever driver the routing rules select, so the module is
// driver-agnostic and links neither iceoryx nor shm_ext.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "cpp/transport/loaned_sample.h"
#include "cpp/transport/transport_handle.h"

#include "bench_options.h"
#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::shm_loan_v2 {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "ShmLoanV2PublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;

#define IBMW_SHM_LOAN_V2_SENDER_DECL(LBL, N, ALIAS) \
  ibmw::transport::SenderRef sender_##N##_;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_LOAN_V2_SENDER_DECL)
#undef IBMW_SHM_LOAN_V2_SENDER_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace ibmw::samples::transport_benchmark::shm_loan_v2
