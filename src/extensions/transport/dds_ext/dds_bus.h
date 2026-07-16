// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>

namespace ibmw::extensions::dds_extension {

// Mirrors DdsExtension::TransportPolicy. Kept as a separate enum here
// to avoid a header dependency cycle (dds_bus.h is included by
// dds_extension.h).
enum class DdsBusTransportPolicy {
  kDefault,
  kShmOnly,
  kTcpOnly,
};

struct DdsBusTransportConfig {
  DdsBusTransportPolicy policy = DdsBusTransportPolicy::kDefault;
  // 0 = do not bind a listener (this side is a pure client).
  uint16_t tcp_listen_port = 0;
  // 0 = do not add an initial peer (this side is server-only).
  uint16_t tcp_peer_port = 0;
  std::string tcp_addr = "127.0.0.1";
};

class DdsBus {
 public:
  DdsBus(uint32_t domain_id,
         const std::string& participant_name,
         const DdsBusTransportConfig& transport_config = {});
  ~DdsBus();

  void Initialize();
  void Shutdown();
  void AbandonShutdown();

  eprosima::fastdds::dds::DomainParticipant* GetParticipant() const {
    return participant_;
  }

  uint32_t GetDomainId() const { return domain_id_; }

 private:
  // Apply transport_config_ onto the supplied DomainParticipantQos.
  // Switch between FastDDS builtin transports vs explicit
  // SharedMemoryTransport / TCPv4TransportDescriptor based on policy.
  void ApplyTransportPolicy(eprosima::fastdds::dds::DomainParticipantQos& pqos);

  uint32_t domain_id_;
  std::string participant_name_;
  DdsBusTransportConfig transport_config_;

  eprosima::fastdds::dds::DomainParticipant* participant_ = nullptr;
  eprosima::fastdds::dds::DomainParticipantFactory* factory_ = nullptr;
  bool abandon_shutdown_ = false;
};

}  // namespace ibmw::extensions::dds_extension
