// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include "engine/extension/extension_base.h"
#include "engine/engine.h"
#include "dds_ext/dds_bus.h"

namespace ibmw::extensions::dds_extension {

class DdsExtension : public ExtensionBase {
 public:
  // Transport policy controls which FastDDS transports the
  // DomainParticipant is given. Affects the QoS built in DdsBus.
  //
  //   kDefault  -> FastDDS builtin transports (UDPv4 + SHM). Lets
  //                FastDDS auto-select SHM for intra-host peers and
  //                also engage DataSharing for `is_plain` types. This
  //                is the historical behaviour and remains the default
  //                so any existing yaml without `transport_policy` is
  //                unaffected.
  //   kShmOnly  -> Disable builtin, register only SharedMemoryTransport.
  //                Forces SHM (no UDP discovery beyond loopback). Used
  //                by dds_send_shm to make sure no UDP fallback skews
  //                the SHM-vs-TCP comparison.
  //   kTcpOnly  -> Disable builtin, register only TCPv4Transport on
  //                127.0.0.1 with a deterministic port. Forces all
  //                publish + discovery traffic through the kernel TCP
  //                stack. Used by dds_send_tcp.
  enum class TransportPolicy {
    kDefault,
    kShmOnly,
    kTcpOnly,
  };

  struct Options {
    uint32_t domain_id = 0;
    std::string participant_name = "ibmw_dds_participant";
    // Both fields must be provided together. The XML profile supplies the
    // participant QoS baseline; IBMW still owns domain_id and participant_name.
    std::string xml_profile_file;
    std::string xml_profile_name;
    TransportPolicy transport_policy = TransportPolicy::kDefault;
    // For kTcpOnly only.
    //   tcp_listen_port: this participant binds + listens on this port.
    //                    Set to 0 to act as a pure client (no listener).
    //   tcp_peer_port:   added to initialPeersList so this participant
    //                    actively connects to a peer at tcp_addr:tcp_peer_port.
    //                    Set to 0 to skip (e.g. server-only side).
    // For loopback bench: pub listens on 5100 (peer=5101); sub listens
    // on 5101 (peer=5100).
    uint16_t tcp_listen_port = 0;
    uint16_t tcp_peer_port = 0;
    // Loopback by design -- the bench is intra-host. Override only for
    // multi-host runs.
    std::string tcp_addr = "127.0.0.1";
  };

 public:
  DdsExtension();
  ~DdsExtension() override = default;

  std::string_view Name() const noexcept override { return "dds_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void InitDdsBus();
  void RegisterDdsTransportDriver();
  void RegisterDdsRpcDriver();

 private:
  Options options_;

  std::atomic_bool stop_flag_ = false;

  std::unique_ptr<DdsBus> dds_bus_;
};

}  // namespace ibmw::extensions::dds_extension
