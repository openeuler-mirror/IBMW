// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loaned_ctx publisher module: drives the zero-copy ctx-meta path of
// the iceoryx extension. On every tick it constructs a typed Plain
// bench message (one of BenchPlainPayload{1KB,16KB,256KB,1MB,4MB} per the
// `payload_bytes` yaml setting), then hands it to
// IoxCtxMetaTypedPublisher<T>, which:
//   1. borrows an iceoryx SHM chunk sized to fit the serialized payload,
//   2. CDR-serializes the message DIRECTLY into that chunk (no heap copy,
//      no buffer-array round-trip),
//   3. emits the ctx-meta header (pkg_size + sertype + KV pairs) in front
//      of the payload, and
//   4. publishes.
//
// Receive side stays on the standard IBMW MessageHandler stack, so the
// subscriber side is identical to a normal `OnMessage<PlainT>` flow --
// this sample isolates the publish-side zero-copy improvement.
//
// Static-fan-out design:
//   The module registers all 5 typed publishers in Setup() (one per Plain
//   bench type) and all 5 typed ctx-meta publishers in Run(). PublishLoop
//   then dispatches per-tick to the typed publisher matching
//   opts_.payload_bytes via DispatchByPayloadBytes(). Apples-to-apples:
//   the same 5-way bank is used by iox_send / dds_loaned / dds_send /
//   tcp_send so all 5 latency rows compare a byte-identical message.
//
// The driver pointer is injected by the standalone publisher_app after
// engine.Initialize() returns (same dynamic_cast pattern as iox_loaned_pod
// and the DDS loaned_message_demo).

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "cpp/module_base.h"

#include "bench_options.h"
#include "bench_plain_payloads.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "iceoryx_ext/iox_style_publisher.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanedCtxPublisherModule"};
  }

  // Called by main() after engine.Initialize() so the iceoryx driver is
  // alive and discoverable through GetTransportBus().
  void SetIceoryxDriver(
      ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* d) {
    driver_ = d;
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  // Per-Plain-type slot: holds the standard SenderRef registered in
  // Setup() (so the iceoryx publisher URL gets created in iceoryx_bus_)
  // plus the ctx-meta typed publisher created in Run(). Templated so we
  // can declare one named member per Plain type via the FOR_EACH X-macro
  // below and look up the right slot at publish time.
  template <typename PlainT>
  struct TypedSlot {
    ibmw::transport::SenderRef sender;
    std::shared_ptr<
        ibmw::extensions::iceoryx_extension::IoxCtxMetaTypedPublisher<PlainT>>
        typed_pub;
  };

  ibmw::Runtime core_;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* driver_{nullptr};

  // One named slot per Plain bench type. Generated from the single source
  // of truth IBMW_BENCH_PLAIN_FOR_EACH so adding a new size only requires
  // editing the X-macro.
#define IBMW_PUB_SLOT_DECL(LBL, N, ALIAS) TypedSlot<ALIAS> slot_##N##_;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_SLOT_DECL)
#undef IBMW_PUB_SLOT_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  uint64_t measured_attempts_{0}, measured_success_{0}, measured_fail_{0};
  uint32_t first_attempt_seq_{0}, last_attempt_seq_{0};
  uint32_t first_success_seq_{0}, last_success_seq_{0};
  bool have_attempt_{false}, have_success_{false};
  bool completed_send_window_{false};
};

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
