// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/dds_bus.h"
#include "dds_ext/global.h"

#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#if __has_include(<fastdds/rtps/transport/TCPv4TransportDescriptor.hpp>)
#include <fastdds/rtps/transport/TCPv4TransportDescriptor.hpp>
#else
#include <fastdds/rtps/transport/TCPv4TransportDescriptor.h>
#endif
#if __has_include(<fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>)
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>
#else
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.h>
#endif
#if __has_include(<fastdds/utils/IPLocator.hpp>)
#include <fastdds/utils/IPLocator.hpp>
#else
#include <fastrtps/utils/IPLocator.h>
#endif

namespace ibmw::extensions::dds_extension {

#if __has_include(<fastdds/rtps/transport/TCPv4TransportDescriptor.hpp>)
namespace dds_bus_tcp_rtps = eprosima::fastdds::rtps;
#else
namespace dds_bus_tcp_rtps = eprosima::fastdds::rtps;
#endif

#if __has_include(<fastdds/utils/IPLocator.hpp>)
namespace dds_bus_ip_rtps = eprosima::fastdds::rtps;
#else
namespace dds_bus_ip_rtps = eprosima::fastrtps::rtps;
#endif

DdsBus::DdsBus(uint32_t domain_id,
               const std::string& participant_name,
               const DdsBusTransportConfig& transport_config)
    : domain_id_(domain_id),
      participant_name_(participant_name),
      transport_config_(transport_config) {}

DdsBus::~DdsBus() {
  Shutdown();
}

void DdsBus::Initialize() {
  factory_ = eprosima::fastdds::dds::DomainParticipantFactory::get_instance();

  eprosima::fastdds::dds::DomainParticipantQos pqos;
  pqos.name(participant_name_);

  ApplyTransportPolicy(pqos);

  participant_ = factory_->create_participant(domain_id_, pqos);

  if (participant_ == nullptr) {
    MW_ERROR("Failed to create DDS DomainParticipant");
    throw std::runtime_error("Failed to create DDS DomainParticipant");
  }

  MW_INFO("DDS Bus initialized, domain_id: {}, participant_name: {}",
             domain_id_, participant_name_);
}

void DdsBus::ApplyTransportPolicy(
    eprosima::fastdds::dds::DomainParticipantQos& pqos) {
  using eprosima::fastdds::rtps::SharedMemTransportDescriptor;
  using dds_bus_tcp_rtps::TCPv4TransportDescriptor;
  using dds_bus_ip_rtps::IPLocator;
  using dds_bus_ip_rtps::Locator_t;

  switch (transport_config_.policy) {
    case DdsBusTransportPolicy::kDefault: {
      // Leave FastDDS builtin transports (UDPv4 + SHM) untouched.
      MW_INFO("DDS Bus transport policy: default (builtin UDPv4 + SHM)");
      break;
    }
    case DdsBusTransportPolicy::kShmOnly: {
      pqos.transport().use_builtin_transports = false;
      auto shm = std::make_shared<SharedMemTransportDescriptor>();
      pqos.transport().user_transports.push_back(shm);
      MW_INFO("DDS Bus transport policy: shm_only");
      break;
    }
    case DdsBusTransportPolicy::kTcpOnly: {
      pqos.transport().use_builtin_transports = false;
      auto tcp = std::make_shared<TCPv4TransportDescriptor>();
      if (transport_config_.tcp_listen_port != 0) {
        tcp->add_listener_port(transport_config_.tcp_listen_port);
      }
      tcp->set_WAN_address(transport_config_.tcp_addr);
      pqos.transport().user_transports.push_back(tcp);

      // For TCP-only RTPS we MUST advertise where this participant
      // listens for discovery (metatraffic) -- otherwise the remote
      // peer cannot route PDP back. Also clear default multicast
      // locator (UDP) which we no longer have a transport for.
      if (transport_config_.tcp_listen_port != 0) {
        Locator_t listen_loc;
        listen_loc.kind = LOCATOR_KIND_TCPv4;
        IPLocator::setIPv4(listen_loc, transport_config_.tcp_addr);
        IPLocator::setPhysicalPort(listen_loc, transport_config_.tcp_listen_port);
        IPLocator::setLogicalPort(listen_loc, transport_config_.tcp_listen_port);
        pqos.wire_protocol().builtin.metatrafficUnicastLocatorList.push_back(listen_loc);
      }

      // Add the remote peer (other side's listener) so this participant
      // actively connects out for discovery + RTPS data.
      if (transport_config_.tcp_peer_port != 0) {
        Locator_t initial_peer;
        initial_peer.kind = LOCATOR_KIND_TCPv4;
        IPLocator::setIPv4(initial_peer, transport_config_.tcp_addr);
        IPLocator::setPhysicalPort(initial_peer, transport_config_.tcp_peer_port);
        IPLocator::setLogicalPort(initial_peer, transport_config_.tcp_peer_port);
        pqos.wire_protocol().builtin.initialPeersList.push_back(initial_peer);
      }

      MW_INFO("DDS Bus transport policy: tcp_only, addr={}, listen_port={}, peer_port={}",
                 transport_config_.tcp_addr,
                 transport_config_.tcp_listen_port,
                 transport_config_.tcp_peer_port);
      break;
    }
  }
}

void DdsBus::Shutdown() {
  if (abandon_shutdown_) {
    participant_ = nullptr;
    MW_INFO("DDS Bus shutdown abandoned; OS will reclaim FastDDS resources.");
    return;
  }

  if (participant_ != nullptr && factory_ != nullptr) {
    factory_->delete_participant(participant_);
    participant_ = nullptr;
    MW_INFO("DDS Bus shutdown");
  }
}

void DdsBus::AbandonShutdown() {
  abandon_shutdown_ = true;
}

}  // namespace ibmw::extensions::dds_extension
