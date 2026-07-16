// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// dds_loaned publisher module: drives the DDS LoanedMessage zero-copy
// path of the dds_extension. Per-tick it constructs a typed Plain bench
// message (one of BenchPlainPayload{1KB,16KB,256KB,1MB,4MB} per the
// `payload_bytes` yaml setting), borrows a loaned message from the
// FastDDS DataWriter via Ros2StylePublisher::borrow_loaned_message(),
// fills the bench fields directly into the loaned buffer, and publishes
// via std::move. The DDS LoanedMessage path bypasses the engine CDR
// pipeline entirely:
//   1. FastDDS DataWriter loans an SHM chunk (DataSharing/iceoryx-iox
//      backend) sized to sizeof(PlainT),
//   2. user code writes Plain fields directly into the chunk,
//      no serialization step,
//   3. publish moves the loan back to FastDDS, no copy.
//
// IsPlainType + can_loan_messages must hold for every type in the bank
// (validated at Run() time). This is the apples-to-DDS counterpart of
// iox_loaned_pod -- both are pure end-to-end zero-copy, but on
// different transports.
//
// Static fan-out design (apples-to-apples with iox_loaned_ctx /
// iox_send / dds_send / tcp_send):
//   The module registers all 5 typed publishers in Setup() (engine-side
//   RegisterSendType callback into the dds driver, which then registers
//   each type with the FastDDS Participant + creates a DataWriter); in
//   Run() it wraps each DataWriter in a Ros2StylePublisher<PlainT>;
//   PublishLoop dispatches per-tick to the publisher matching
//   opts_.payload_bytes via an inlined if-chain X-macro. Same bank used
//   across all 5 non-POD samples so all rows compare a byte-identical
//   message.

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "cpp/module_base.h"

// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/ros2_style_api.h"
// IBMW_R1_EXEMPT_END

#include "bench_options.h"
#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::dds_loaned {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "DdsLoanedPublisherModule"};
  }

  // The DDS driver is injected from the standalone main between
  // engine.Initialize() and engine.Start(); its raw pointer is stable
  // for the lifetime of the engine.
  void SetDdsDriver(
      ibmw::extensions::dds_extension::DdsTransportDriver* d) {
    driver_ = d;
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  // Per-typed-slot state. The SenderRef holds the engine-side publisher
  // handle returned from GetPublisher(kTopic) -- de-duplicated across
  // all 5 RegisterSendType<T> calls because they share the topic name.
  // typed_pub is the FastDDS-backed Ros2StylePublisher<T> created in
  // Run() (after the dds driver has built the underlying DataWriter).
  template <typename PlainT>
  struct TypedSlot {
    ibmw::transport::SenderRef sender;
    std::shared_ptr<ibmw::extensions::dds_extension::Ros2StylePublisher<PlainT>>
        typed_pub;
  };

  ibmw::Runtime core_;
  ibmw::extensions::dds_extension::DdsTransportDriver* driver_{nullptr};

  // X-macro fan-out: one named slot per Plain bench type, generated
  // from the same single source of truth (IBMW_BENCH_PLAIN_FOR_EACH)
  // used by iox_loaned_ctx / dds_send / tcp_send so the bank stays in
  // lock-step.
#define IBMW_PUB_SLOT_DECL(LBL, N, ALIAS) TypedSlot<ALIAS> slot_##N##_;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_SLOT_DECL)
#undef IBMW_PUB_SLOT_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  bool WaitForSelectedPublisherMatch();
};

}  // namespace ibmw::samples::transport_benchmark::dds_loaned
