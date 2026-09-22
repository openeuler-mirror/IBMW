// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/topic_subscription.h"
#include <atomic>
#include <thread>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
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

namespace {
using namespace std::chrono_literals;
namespace dds = eprosima::fastdds::dds;
namespace compat = ibmw::extensions::dds_extension::fastdds_compat;
using namespace ibmw::discovery;
using Clock = std::chrono::steady_clock;
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

TEST(DdsTopicSubscription, ReplacesReaderForOwnershipAndPreservesSameQosReconnect) {
  const uint32_t domain = 140 + ::getpid() % 50;
  const std::string topic_name = "ownership_" + std::to_string(::getpid());
  Participant participant(dds::DomainParticipantFactory::get_instance()->create_participant(
      domain, dds::PARTICIPANT_QOS_DEFAULT));
  ASSERT_TRUE(participant);
  auto type = MakeType();
  ASSERT_EQ(type.register_type(participant.get()), compat::kReturnCodeOk);
  auto* topic = participant->create_topic(topic_name, type.get_type_name(), dds::TOPIC_QOS_DEFAULT);
  auto* publisher = participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
  ASSERT_NE(topic, nullptr);
  ASSERT_NE(publisher, nullptr);
  auto discovery = CreateDiscoveryProvider(DiscoveryBackend::kDds);
  ASSERT_TRUE(discovery);
  discovery->Start({.backend = DiscoveryBackend::kDds, .domain_id = domain});
  const auto wait = [](auto predicate) {
    const auto deadline = Clock::now() + 6s;
    do {
      if (predicate()) return true;
      std::this_thread::sleep_for(20ms);
    } while (Clock::now() < deadline);
    return false;
  };
  std::atomic<unsigned> received{0};
  ibmw::runtime::core::topic_io::TopicSubscription subscription({
      .source = {.backend = DiscoveryBackend::kDds, .domain_id = domain}, .topic = topic_name,
      .qos = {.policies = {{"reliability", "reliable"}}}});
  subscription.Start([&](auto&&) { ++received; });
  // Both directions, plus a same-QoS reconnect, in one persistent observer.
  for (auto ownership : {dds::EXCLUSIVE_OWNERSHIP_QOS, dds::SHARED_OWNERSHIP_QOS,
                         dds::SHARED_OWNERSHIP_QOS, dds::EXCLUSIVE_OWNERSHIP_QOS}) {
    dds::DataWriterQos qos;
    qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
    qos.ownership().kind = ownership;
    auto* writer = publisher->create_datawriter(topic, qos);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(wait([&] {
      subscription.Refresh();
      dds::PublicationMatchedStatus status;
      writer->get_publication_matched_status(status);
      return status.current_count == 1;
    }));
    const auto before = received.load();
    ibmw::dds::GenericMsg message;
    ASSERT_TRUE(wait([&] { writer->write(&message); return received.load() > before; }));
    // Repeated refreshes must not add another reader.
    for (int i = 0; i != 3; ++i) subscription.Refresh();
    dds::PublicationMatchedStatus status;
    writer->get_publication_matched_status(status);
    EXPECT_EQ(status.current_count, 1);
    ASSERT_EQ(publisher->delete_datawriter(writer), compat::kReturnCodeOk);
    ASSERT_TRUE(wait([&] {
      const auto snapshot = discovery->Query(Clock::now());
      return std::none_of(snapshot.endpoints.begin(), snapshot.endpoints.end(),
          [&](const auto& endpoint) { return endpoint.wire_topic_name == topic_name &&
                                           endpoint.role == EndpointRole::kPublisher; });
    }));
    // Let the subscription's independent discovery cache observe the removal.
    const auto deadline = Clock::now() + 200ms;
    while (Clock::now() < deadline) { subscription.Refresh(); std::this_thread::sleep_for(20ms); }
  }
  subscription.Stop();
}
}  // namespace
