// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/dds_bus.h"
#include "dds_ext/fastdds_compat.h"
#include "dds_ext/global.h"

#include <stdexcept>

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

DdsBus::DdsBus(uint32_t domain_id,
               const std::string& participant_name,
               const DdsBusTransportConfig& transport_config,
               const DdsBusXmlProfileConfig& xml_profile_config)
    : DdsBus(domain_id, participant_name, transport_config) {
  xml_profile_config_ = xml_profile_config;
}

DdsBus::~DdsBus() {
  Shutdown();
}

void DdsBus::Initialize() {
  factory_ = eprosima::fastdds::dds::DomainParticipantFactory::get_instance();

  eprosima::fastdds::dds::DomainParticipantQos pqos;
  const bool has_xml_file = !xml_profile_config_.file.empty();
  const bool has_xml_profile = !xml_profile_config_.participant_profile.empty();
  if (has_xml_file != has_xml_profile) {
    MW_ERROR("DDS XML configuration requires both xml_profile_file and xml_profile_name");
    throw std::runtime_error(
        "DDS XML configuration requires both xml_profile_file and xml_profile_name");
  }

  if (has_xml_file) {
    if (factory_->load_XML_profiles_file(xml_profile_config_.file) !=
        fastdds_compat::kReturnCodeOk) {
      MW_ERROR("Failed to load DDS XML profile file: {}",
               xml_profile_config_.file);
      throw std::runtime_error("Failed to load DDS XML profile file: " +
                               xml_profile_config_.file);
    }

    if (factory_->get_participant_qos_from_profile(
            xml_profile_config_.participant_profile, pqos) !=
        fastdds_compat::kReturnCodeOk) {
      MW_ERROR("Failed to find DDS participant profile '{}' in {}",
               xml_profile_config_.participant_profile,
               xml_profile_config_.file);
      throw std::runtime_error("Failed to find DDS participant profile: " +
                               xml_profile_config_.participant_profile);
    }

    MW_INFO("DDS XML loaded, file: {}, participant_profile: {}",
            xml_profile_config_.file,
            xml_profile_config_.participant_profile);
  }

  pqos.name(participant_name_);

  ApplyTransportPolicy(pqos);

  MW_INFO("DDS transports resolved, builtin: {}, user_count: {}",
          pqos.transport().use_builtin_transports,
          pqos.transport().user_transports.size());

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
      // Preserve XML transport settings when a profile was selected. Without
      // XML this retains Fast DDS' historical builtin UDPv4 + SHM behavior.
      MW_INFO("DDS Bus transport policy: default");
      break;
    }
    case DdsBusTransportPolicy::kShmOnly: {
      pqos.transport().use_builtin_transports = false;
      pqos.transport().user_transports.clear();
      pqos.wire_protocol().builtin.metatrafficUnicastLocatorList.clear();
      pqos.wire_protocol().builtin.metatrafficMulticastLocatorList.clear();
      pqos.wire_protocol().builtin.initialPeersList.clear();
      pqos.wire_protocol().default_unicast_locator_list.clear();
      pqos.wire_protocol().default_multicast_locator_list.clear();
      auto shm = std::make_shared<SharedMemTransportDescriptor>();
      pqos.transport().user_transports.push_back(shm);
      MW_INFO("DDS Bus transport policy: shm_only");
      break;
    }
    case DdsBusTransportPolicy::kTcpOnly: {
      pqos.transport().use_builtin_transports = false;
      pqos.transport().user_transports.clear();
      pqos.wire_protocol().builtin.metatrafficUnicastLocatorList.clear();
      pqos.wire_protocol().builtin.metatrafficMulticastLocatorList.clear();
      pqos.wire_protocol().builtin.initialPeersList.clear();
      pqos.wire_protocol().default_unicast_locator_list.clear();
      pqos.wire_protocol().default_multicast_locator_list.clear();
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
