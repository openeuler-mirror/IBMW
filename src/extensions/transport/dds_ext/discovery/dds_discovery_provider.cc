// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_discovery_provider.h"

#include <exception>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "../fastdds_compat.h"
#include "../detail/qos_observation.h"
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>

namespace ibmw::discovery {
namespace {
namespace dds = eprosima::fastdds::dds;
namespace rtps = ibmw::extensions::dds_extension::fastdds_compat::rtps;

std::string GuidString(const rtps::GUID_t& guid) {
  std::ostringstream stream;
  stream << guid;
  return stream.str();
}

class Provider final : public DiscoveryProvider, private dds::DomainParticipantListener {
 public:
  ~Provider() override { Stop(); }

  void Start(const DiscoveryOptions& options) override {
    if (participant_) throw std::logic_error("DDS discovery is already started.");
    if (options.backend != DiscoveryBackend::kDds) throw std::invalid_argument("Invalid DDS discovery source.");
    source_ = DescribeSource(options);
    auto* factory = dds::DomainParticipantFactory::get_instance();
    auto qos = factory->get_default_participant_qos();
    qos.name("ibmw_discovery");
    participant_ = factory->create_participant(options.domain_id, qos, this);
    if (!participant_) throw std::runtime_error("Cannot create DDS participant for domain " + source_.domain);
  }

  DiscoverySnapshot Query(std::chrono::steady_clock::time_point deadline) override {
    if (!participant_) throw std::logic_error("DDS discovery is not started.");
    std::this_thread::sleep_until(deadline);
    std::lock_guard lock(mutex_);
    if (callback_error_) std::rethrow_exception(callback_error_);
    DiscoverySnapshot result;
    for (const auto& [id, endpoint] : endpoints_) result.endpoints.push_back(endpoint);
    result.sources.push_back({.source = source_, .status = DiscoveryStatus::kReady,
                              .observed_at = std::chrono::system_clock::now()});
    return result;
  }

  void Stop() noexcept override {
    if (participant_) {
      dds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
      participant_ = nullptr;
    }
    std::lock_guard lock(mutex_);
    endpoints_.clear();
    callback_error_ = nullptr;
  }

 private:
  template <typename Function>
  void OnDiscovery(Function&& function) noexcept {
    std::lock_guard lock(mutex_);
    try { function(); } catch (...) { callback_error_ = std::current_exception(); }
  }

  void RemoveParticipant(const rtps::GUID_t& guid) {
    const auto participant = GuidString(guid);
    std::erase_if(endpoints_, [&](const auto& item) { return item.second.participant_id == participant; });
  }

  void Update(const rtps::GUID_t& guid, const rtps::GUID_t& participant,
              const char* topic, const char* type, EndpointRole role,
              ibmw::transport::QosDescriptor qos) {
    const auto id = GuidString(guid);
    DiscoveredEndpoint value{.source = source_, .participant_id = GuidString(participant),
                             .endpoint_id = id, .role = role};
    value.wire_topic_name = topic;
    value.wire_type_name = type;
    value.serialization_type = "cdr";
    value.qos = std::move(qos);
    value.available_drivers = {"dds"};
    value.hidden = value.wire_topic_name.starts_with('_') || value.wire_topic_name.find("/_") != std::string::npos;
    endpoints_.insert_or_assign(id, std::move(value));
  }

#if IBMW_FASTDDS_V3_PLUS
  void on_participant_discovery(dds::DomainParticipant*, rtps::ParticipantDiscoveryStatus reason,
      const rtps::ParticipantBuiltinTopicData& info, bool& ignored) override {
    ignored = false;
    OnDiscovery([&] {
      if (reason == rtps::ParticipantDiscoveryStatus::REMOVED_PARTICIPANT ||
          reason == rtps::ParticipantDiscoveryStatus::DROPPED_PARTICIPANT ||
          reason == rtps::ParticipantDiscoveryStatus::IGNORED_PARTICIPANT) RemoveParticipant(info.guid);
    });
  }
  void on_data_writer_discovery(dds::DomainParticipant*, rtps::WriterDiscoveryStatus reason,
      const rtps::PublicationBuiltinTopicData& info, bool& ignored) override {
    ignored = false;
    OnDiscovery([&] {
      if (reason == rtps::WriterDiscoveryStatus::REMOVED_WRITER ||
          reason == rtps::WriterDiscoveryStatus::IGNORED_WRITER) {
        endpoints_.erase(GuidString(info.guid));
      } else {
        Update(info.guid, info.participant_guid, info.topic_name.c_str(), info.type_name.c_str(),
               EndpointRole::kPublisher,
               ibmw::extensions::dds_extension::detail::observation::Discovery(info, true));
      }
    });
  }
  void on_data_reader_discovery(dds::DomainParticipant*, rtps::ReaderDiscoveryStatus reason,
      const rtps::SubscriptionBuiltinTopicData& info, bool& ignored) override {
    ignored = false;
    OnDiscovery([&] {
      if (reason == rtps::ReaderDiscoveryStatus::REMOVED_READER ||
          reason == rtps::ReaderDiscoveryStatus::IGNORED_READER) {
        endpoints_.erase(GuidString(info.guid));
      } else {
        Update(info.guid, info.participant_guid, info.topic_name.c_str(), info.type_name.c_str(),
               EndpointRole::kSubscriber,
               ibmw::extensions::dds_extension::detail::observation::Discovery(info, false));
      }
    });
  }
#else
  template <typename Info>
  void UpdateProxy(const Info& info, EndpointRole role) {
    const auto& endpoint = info.info;
    const rtps::GUID_t participant(endpoint.guid().guidPrefix, rtps::c_EntityId_RTPSParticipant);
    Update(endpoint.guid(), participant, endpoint.topicName().c_str(), endpoint.typeName().c_str(), role,
           ibmw::extensions::dds_extension::detail::observation::Discovery(endpoint, role == EndpointRole::kPublisher));
  }
  void on_participant_discovery(dds::DomainParticipant*, rtps::ParticipantDiscoveryInfo&& info) override {
    OnDiscovery([&] {
      if (info.status == rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT ||
          info.status == rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT) RemoveParticipant(info.info.m_guid);
    });
  }
  void on_publisher_discovery(dds::DomainParticipant*, rtps::WriterDiscoveryInfo&& info) override {
    OnDiscovery([&] {
      if (info.status == rtps::WriterDiscoveryInfo::REMOVED_WRITER) endpoints_.erase(GuidString(info.info.guid()));
      else UpdateProxy(info, EndpointRole::kPublisher);
    });
  }
  void on_subscriber_discovery(dds::DomainParticipant*, rtps::ReaderDiscoveryInfo&& info) override {
    OnDiscovery([&] {
      if (info.status == rtps::ReaderDiscoveryInfo::REMOVED_READER) endpoints_.erase(GuidString(info.info.guid()));
      else UpdateProxy(info, EndpointRole::kSubscriber);
    });
  }
#endif

  DiscoverySource source_;
  dds::DomainParticipant* participant_ = nullptr;
  std::mutex mutex_;
  std::map<std::string, DiscoveredEndpoint> endpoints_;
  std::exception_ptr callback_error_;
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT
ibmw::discovery::DiscoveryPluginV1 IbmwDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = DiscoveryBackend::kDds,
          .create = [] { return static_cast<DiscoveryProvider*>(new Provider); },
          .destroy = [](DiscoveryProvider* provider) noexcept { delete provider; }};
}
