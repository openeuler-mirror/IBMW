// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_send publisher module: drives the SHM-but-no-loaned baseline path
// of the iceoryx extension.
//
// On every tick it constructs a typed Plain bench message (one of
// BenchPlainPayload{1KB,16KB,256KB,1MB,4MB} per the `payload_bytes`
// yaml setting) and publishes it through the standard
// `ibmw::transport::Send(publisher, msg)` call. Inside the engine +
// iceoryx driver, this means:
//   1. the engine CDR-serializes the message into a heap-allocated
//      buffer (BufferArray + FlatBuffer allocator on the heap),
//   2. the iceoryx driver borrows an SHM chunk sized to the serialized
//      payload + ctx-meta header, and memcpy's the buffer into it,
//   3. publishes.
//
// The transport (iceoryx SHM) and the wire format (pkg_size + sertype +
// KV header + CDR payload) are IDENTICAL to iox_loaned_ctx -- the only
// difference is the extra heap alloc + memcpy on the publish side.
// Subtracting iox_send latency from iox_loaned_ctx latency therefore
// isolates the publish-side loaned-vs-heap-memcpy contribution.
//
// Static-fan-out design (apples-to-apples with iox_loaned_ctx):
//   The module registers all 5 typed publishers (one per Plain bench
//   type) in Setup(); PublishLoop dispatches per-tick to the publisher
//   matching opts_.payload_bytes via an inlined if-chain X-macro. The
//   same bank is shared across iox_loaned_ctx / iox_send / dds_loaned /
//   dds_send / tcp_send so all rows compare a byte-identical message.
//
// No driver pointer is needed because no extension-only API is called.

#pragma once

#include <atomic>
#include <optional>
#include <thread>

#include "cpp/module_base.h"
#include "cpp/transport/transport_factory.h"

#include "bench_options.h"
#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark::iox_send {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxSendPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;

  // One typed SenderAccess<T> per Plain bench type, materialized via
  // ibmw::transport::publisher_for<T>(rt, topic) in Setup(). Stored in
  // std::optional because SenderAccess<T> is move-only and not
  // default-constructible (public-API tightening
  // of the iox_send sample to use the factory facade; semantics
  // unchanged vs. the prior GetPublisher + RegisterSendType<T> form).
#define IBMW_PUB_SENDER_DECL(LBL, N, ALIAS) \
  std::optional<ibmw::transport::SenderAccess<ALIAS>> sender_##N##_;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_SENDER_DECL)
#undef IBMW_PUB_SENDER_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace ibmw::samples::transport_benchmark::iox_send
