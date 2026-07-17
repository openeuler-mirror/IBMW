// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// dds_send_tcp publisher module: drives the standard non-loaned DDS Send
// path of the dds extension.
//
// On every tick it constructs a typed non-plain bench message (one of
// dds_send_bank::BenchSendPayload{1KB,16KB,256KB,1MB,4MB} per the
// `payload_bytes` yaml setting) and publishes it through the standard
// `ibmw::transport::Send(publisher, msg)` call.
//
// CRITICAL: dds_send_tcp uses bench_send_payloads.h (a parallel non-Plain
// type bank) instead of bench_plain_payloads.h. The Plain bank opts
// types into PlainDdsPubSubType<T> via IBMW_DDS_PLAIN_TYPE which sets
// is_plain()==true and lets FastDDS engage DataSharing/SHM zero-copy.
// dds_send_tcp wants the OPPOSITE -- the standard publish path (FastCDR
// serialise + transport buffer copy) so the matrix can isolate the
// DataSharing benefit. Sharing one type across both samples would
// silently re-engage DataSharing in dds_send_tcp (global trait
// specialisation reaches every TU). See bench_send_payloads.h header
// comment for details.
//
// Inside the engine + dds driver, the standard Send path means:
//   1. the engine CDR-serializes the message into a heap-allocated
//      buffer (BufferArray + FlatBuffer allocator on the heap),
//   2. the dds driver hands the buffer to FastDDS via
//      GenericDdsPubSubType<T>::serialize (which copies into a
//      SerializedPayload_t -- and from there FastDDS routes it over
//      its only registered transport: TCPv4 (transport_policy:
//      tcp_only forces builtin SHM/UDP off)),
//   3. publishes.
//
// The transport (DDS-over-TCP) and the wire format (CDR, identical to
// dds_send_shm and dds_loaned on the receiver side after deserialize)
// isolate the kernel-TCP socket cost vs dds_send_shm. Subtracting
// dds_send_shm latency from dds_send_tcp latency isolates the
// SHM-vs-TCP transport contribution.
//
// Static-fan-out design (apples-to-apples with dds_loaned):
//   The module registers all 5 typed publishers (one per Plain bench
//   type) in Setup(); PublishLoop dispatches per-tick to the publisher
//   matching opts_.payload_bytes via an inlined if-chain X-macro. The
//   same bank is shared across iox_loaned_ctx / iox_send / dds_loaned /
//   dds_send_tcp / tcp_send so all rows compare a byte-identical message.
//
// No driver pointer is needed because no extension-only API is called.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"

#include "bench_options.h"
#include "bench_send_payloads.h"

namespace ibmw::samples::transport_benchmark::dds_send_tcp {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "DdsSendPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;

  // One named SenderRef per non-plain bench type. Generated from
  // IBMW_BENCH_SEND_FOR_EACH (the dds_send_tcp-specific bank) so the static
  // fan-out stays in lock-step with iox_loaned_ctx / dds_loaned / etc.
  // even though the underlying types differ (non-Plain vs Plain).
#define IBMW_PUB_SENDER_DECL(LBL, N, ALIAS) \
  ibmw::transport::SenderRef sender_##N##_;
  IBMW_BENCH_SEND_FOR_EACH(IBMW_PUB_SENDER_DECL)
#undef IBMW_PUB_SENDER_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace ibmw::samples::transport_benchmark::dds_send_tcp
