// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loan_v2 publisher module: drives the IBMW user-level zero-copy
// loan fast lane (Phase 7 §7.3, Step 3) end-to-end against typed Plain
// payloads.
//
// Compared to the three reference baselines:
//
//   iox_send         RegisterSendType + ibmw::transport::Send     (heap+memcpy)
//   iox_loaned_ctx   RegisterSendType + ctx-meta loan via legacy   (kCtxMetaThenPayload)
//   iox_loaned_pod   IoxRawPodPublisher (extension-only API)       (kPodOnly, no encap)
//
// iox_loan_v2 occupies the slot:
//
//   RegisterSendType + LoanedSample<T>::Borrow + ::Publish         (kPodOnly, encapsulated)
//
// i.e. it goes through the public IBMW SenderRef::Loan / PublishLoaned
// API surface (which is what user applications would use), reaching the
// iceoryx driver via the dispatcher's try-each-driver Loan path.
//
// Topic registration on the iceoryx side requires BOTH:
//   1) RegisterSendType<T>(sender_) -- so dispatcher.Loan() can find a
//      SendTypeWrapper for the (topic, msg_type) pair (it asserts
//      registration before walking the driver list).
//   2) driver_->EnsurePodTopic(topic, prefixed_msg_type, hint) -- so
//      the iceoryx driver's POD publisher map has an entry for the
//      "/transport_zc_pod/<topic>/<msg_type>" URL the LoanSample()
//      hot path looks up.
// Both happen during pre-start preparation; neither is on the per-tick path.
//
// Static fan-out by yaml-driven payload_bytes uses the same X-macro
// (IBMW_BENCH_PLAIN_FOR_EACH) iox_send / iox_loaned_ctx / dds_* / etc.
// already share, so any latency delta against those rows is
// attributable solely to the publish-side API path.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "cpp/transport/loaned_sample.h"
#include "cpp/transport/transport_handle.h"

#include "bench_options.h"
#include "bench_plain_payloads.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::iox_loan_v2 {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanV2PublisherModule"};
  }

  // Injected after Engine::Initialize() and used only by PreparePodTopics().
  // The publish hot loop never touches the driver.
  void SetIceoryxDriver(
      ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* d) {
    driver_ = d;
  }

  // Registers all POD topics while the engine and PoshRuntime are initialized,
  // but before Engine::Start() transitions the driver to its hot state.
  bool PreparePodTopics();

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* driver_{nullptr};
  bool pod_topics_prepared_{false};

  // One named SenderRef per Plain bench type. Same shape as iox_send so
  // the only per-row difference vs. iox_send is the publish-side call
  // (LoanedSample<T>::Borrow + Publish vs. Send).
#define IBMW_LOAN_V2_SENDER_DECL(LBL, N, ALIAS) \
  ibmw::transport::SenderRef sender_##N##_;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_LOAN_V2_SENDER_DECL)
#undef IBMW_LOAN_V2_SENDER_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace ibmw::samples::transport_benchmark::iox_loan_v2
