// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// net_send_tcp publisher module: drives the standard `Send<T>` path of
// the IBMW native net_extension over its raw asio TCP transport
// driver. This is THE non-DDS, non-iceoryx baseline -- the only
// kernel-network row in the 7-sample matrix.
//
// On every tick it constructs a typed non-plain bench message (one of
// dds_send_bank::BenchSendPayload{1KB,16KB,256KB,1MB,4MB} per the
// `payload_bytes` yaml setting) and publishes it through the standard
// `ibmw::transport::Send(publisher, msg)` call.
//
// CRITICAL: net_send_tcp uses bench_send_payloads.h (the non-Plain type
// bank shared with dds_send_shm / dds_send_tcp) so the wire payload
// is byte-identical across all three Send-path rows. The Plain bank
// (bench_plain_payloads.h) opts types into IBMW_DDS_PLAIN_TYPE which
// is a DDS-only construct -- net_extension's TCP driver doesn't read
// it. Reusing the non-Plain bank keeps every Send<T> row CDR-serialized
// via the engine's MessageTypeSupport "cdr" registration, regardless
// of underlying transport.
//
// Inside the engine + net tcp driver, the standard Send path means:
//   1. the engine CDR-serializes the message into a heap-allocated
//      BufferArray (FlatBuffer allocator on the heap) via
//      MessageTypeSupport::DefaultSerializationType()=="cdr",
//   2. the tcp driver wraps it in a custom binary frame
//      (pattern + serialization_type + ctx-meta KV + payload bytes)
//      and writes it to a TCP socket connected to the configured
//      server_url_list[0],
//   3. publishes.
//
// The transport (loopback TCP, kernel network stack -- no SHM at all)
// and the framing (custom binary, NOT RTPS) differ from every DDS
// row, but the userland Send<T> API and the CDR payload bytes are
// identical. Subtracting net_send_tcp latency from dds_send_tcp
// latency isolates the RTPS framing/discovery overhead vs raw asio.
//
// Static-fan-out design (apples-to-apples with the other Send rows):
//   The module registers all 5 typed publishers (one per non-Plain
//   bench type) in Setup(); PublishLoop dispatches per-tick to the
//   publisher matching opts_.payload_bytes via an inlined if-chain
//   X-macro. The same bank is shared across dds_send_shm /
//   dds_send_tcp / net_send_tcp so all three Send rows compare a
//   byte-identical message.
//
// No driver pointer is needed because no extension-only API is called.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"

#include "bench_options.h"
#include "bench_send_payloads.h"

namespace ibmw::samples::transport_benchmark::net_send_tcp {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "NetSendTcpPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;

  // One named SenderRef per non-plain bench type. Generated from
  // IBMW_BENCH_SEND_FOR_EACH (the net_send_tcp-specific bank) so the static
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

}  // namespace ibmw::samples::transport_benchmark::net_send_tcp
