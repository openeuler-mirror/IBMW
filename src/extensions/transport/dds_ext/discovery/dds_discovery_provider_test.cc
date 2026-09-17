// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_discovery_provider.h"
#include "dds_ext/fastdds_compat.h"
#include "DdsGenericMsg.hpp"
#if IBMW_FASTDDS_V3_PLUS
#include "DdsGenericMsgPubSubTypes.hpp"
#else
#include "DdsGenericMsgCdrAux.ipp"
#include "dds_types/util/generic_dds_pubsub_type.h"
#endif

#include <algorithm>
#include <memory>
#include <unistd.h>
#include <gtest/gtest.h>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

namespace ibmw::discovery {
namespace {
using namespace std::chrono_literals;
namespace dds = eprosima::fastdds::dds;
namespace compat = ibmw::extensions::dds_extension::fastdds_compat;
using Clock = std::chrono::steady_clock;

auto MakeProvider() {
  const auto api = IbmwDiscoveryEntryV1();
  return std::unique_ptr<DiscoveryProvider, decltype(api.destroy)>(api.create(), api.destroy);
}

struct ParticipantDeleter {
  void operator()(dds::DomainParticipant* participant) const {
    participant->delete_contained_entities();
    dds::DomainParticipantFactory::get_instance()->delete_participant(participant);
  }
};
using Participant = std::unique_ptr<dds::DomainParticipant, ParticipantDeleter>;

dds::TypeSupport MakeType() {
#if IBMW_FASTDDS_V3_PLUS
  return dds::TypeSupport(new ibmw::dds::GenericMsgPubSubType());
#else
  return dds::TypeSupport(new ibmw::dds::GenericDdsPubSubType<ibmw::dds::GenericMsg>(
      "ibmw::dds::dds_::GenericMsg_"));
#endif
}

DiscoverySnapshot WaitFor(DiscoveryProvider& provider, size_t count) {
  const auto deadline = Clock::now() + 5s;
  DiscoverySnapshot snapshot;
  do {
    snapshot = provider.Query(Clock::now() + 50ms);
    if (snapshot.endpoints.size() == count) break;
  } while (Clock::now() < deadline);
  return snapshot;
}

TEST(DdsDiscoveryTest, DiscoversSilentEndpointsMapsQosAndRemovesCachedEndpoints) {
  const auto domain = static_cast<uint32_t>(80 + ::getpid() % 50);
  auto provider = MakeProvider();
  provider->Start({.backend = DiscoveryBackend::kDds, .domain_id = domain});
  Participant participant(dds::DomainParticipantFactory::get_instance()->create_participant(
      domain, dds::PARTICIPANT_QOS_DEFAULT));
  ASSERT_TRUE(participant);
  auto type = MakeType();
  ASSERT_EQ(type.register_type(participant.get()), compat::kReturnCodeOk);
  const auto topic_name = "_ibmw_discovery_test_" + std::to_string(::getpid());
  auto* topic = participant->create_topic(topic_name, type.get_type_name(), dds::TOPIC_QOS_DEFAULT);
  ASSERT_NE(topic, nullptr);
  auto* publisher = participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
  auto* subscriber = participant->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
  ASSERT_NE(publisher, nullptr);
  ASSERT_NE(subscriber, nullptr);
  dds::DataWriterQos writer_qos;
  writer_qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
  writer_qos.durability().kind = dds::TRANSIENT_LOCAL_DURABILITY_QOS;
  writer_qos.deadline().period = {2, 0};
  writer_qos.lifespan().duration = {3, 0};
  writer_qos.liveliness().kind = dds::MANUAL_BY_TOPIC_LIVELINESS_QOS;
  writer_qos.liveliness().lease_duration = {4, 0};
  writer_qos.ownership().kind = dds::EXCLUSIVE_OWNERSHIP_QOS;
  auto* writer = publisher->create_datawriter(topic, writer_qos);
  ASSERT_NE(writer, nullptr);
  dds::DataReaderQos reader_qos;
  reader_qos.reliability().kind = dds::BEST_EFFORT_RELIABILITY_QOS;
  reader_qos.durability().kind = dds::VOLATILE_DURABILITY_QOS;
  auto* reader = subscriber->create_datareader(topic, reader_qos);
  ASSERT_NE(reader, nullptr);
  const auto snapshot = WaitFor(*provider, 2);
  ASSERT_TRUE(snapshot.Succeeded());
  ASSERT_EQ(snapshot.endpoints.size(), 2u);
  EXPECT_EQ(snapshot.endpoints[0].participant_id, snapshot.endpoints[1].participant_id);
  EXPECT_NE(snapshot.endpoints[0].endpoint_id, snapshot.endpoints[1].endpoint_id);
  for (const auto& endpoint : snapshot.endpoints) {
    EXPECT_EQ(endpoint.source.domain, std::to_string(domain));
    EXPECT_EQ(endpoint.wire_topic_name, topic_name);
    EXPECT_EQ(endpoint.wire_type_name, type.get_type_name());
    EXPECT_EQ(endpoint.serialization_type, "cdr");
    EXPECT_TRUE(endpoint.hidden);
    EXPECT_FALSE(endpoint.participant_id.empty());
    EXPECT_EQ(endpoint.available_drivers, std::vector<std::string>{"dds"});
    const auto& qos = endpoint.qos.policies;
    if (endpoint.role == EndpointRole::kPublisher) {
      EXPECT_EQ(qos.at("reliability"), "reliable");
      EXPECT_EQ(qos.at("durability"), "transient_local");
      EXPECT_EQ(qos.at("deadline"), "2 s 0 ns");
      EXPECT_EQ(qos.at("lifespan"), "3 s 0 ns");
      EXPECT_EQ(qos.at("liveliness"), "manual_by_topic");
      EXPECT_EQ(qos.at("liveliness_lease_duration"), "4 s 0 ns");
      EXPECT_EQ(qos.at("ownership"), "exclusive");
    } else {
      EXPECT_EQ(qos.at("reliability"), "best_effort");
      EXPECT_EQ(qos.at("durability"), "volatile");
      EXPECT_EQ(qos.at("deadline"), "infinite");
      EXPECT_EQ(qos.at("liveliness"), "automatic");
      EXPECT_EQ(qos.at("ownership"), "shared");
      EXPECT_FALSE(qos.contains("lifespan"));
    }
  }
  ASSERT_EQ(subscriber->delete_datareader(reader), compat::kReturnCodeOk);
  const auto remaining = WaitFor(*provider, 1);
  ASSERT_EQ(remaining.endpoints.size(), 1u);
  EXPECT_EQ(remaining.endpoints.front().role, EndpointRole::kPublisher);
  participant.reset();
  EXPECT_TRUE(WaitFor(*provider, 0).endpoints.empty());
  provider->Stop();
  provider->Start({.backend = DiscoveryBackend::kDds, .domain_id = domain});
  EXPECT_TRUE(provider->Query(Clock::now() + 100ms).endpoints.empty());
}

TEST(DdsDiscoveryTest, EnforcesProviderLifecycle) {
  auto provider = MakeProvider();
  provider->Stop();
  EXPECT_THROW(provider->Query(Clock::now()), std::logic_error);
  EXPECT_THROW(provider->Start({.backend = DiscoveryBackend::kZenoh}), std::invalid_argument);
  provider->Start({.backend = DiscoveryBackend::kDds, .domain_id = 131});
  EXPECT_THROW(provider->Start({.backend = DiscoveryBackend::kDds, .domain_id = 131}), std::logic_error);
  EXPECT_TRUE(provider->Query(Clock::now()).Succeeded());
  provider->Stop();
  provider->Stop();
}
}  // namespace
}  // namespace ibmw::discovery
