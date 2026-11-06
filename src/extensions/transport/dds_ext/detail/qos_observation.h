// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <nlohmann/json.hpp>

#include "cpp/transport/topic_descriptor.h"
#include "../fastdds_compat.h"

namespace ibmw::extensions::dds_extension::detail {
namespace observation {
namespace dds = eprosima::fastdds::dds;
using Descriptor = ibmw::transport::QosDescriptor;

inline std::string Duration(const auto& value) {
  if (value.is_infinite()) return "infinite";
  return std::to_string(value.seconds) + " s " + std::to_string(value.nanosec) + " ns";
}
inline std::string Boolean(bool value) { return value ? "true" : "false"; }

inline std::string Presentation(const auto& qos) {
  const char* scope = qos.access_scope == dds::INSTANCE_PRESENTATION_QOS ? "instance"
      : qos.access_scope == dds::TOPIC_PRESENTATION_QOS ? "topic"
      : qos.access_scope == dds::GROUP_PRESENTATION_QOS ? "group" : "unknown";
  return nlohmann::json{{"access_scope", scope}, {"coherent_access", qos.coherent_access},
                        {"ordered_access", qos.ordered_access}}.dump();
}

inline std::string DurabilityService(const auto& qos) {
  return nlohmann::json{{"cleanup_delay", Duration(qos.service_cleanup_delay)},
      {"history_kind", static_cast<int>(qos.history_kind)}, {"history_depth", qos.history_depth},
      {"max_samples", qos.max_samples}, {"max_instances", qos.max_instances},
      {"max_samples_per_instance", qos.max_samples_per_instance}}.dump();
}

inline std::string DestinationOrder(const auto& qos) {
  return qos.kind == dds::BY_RECEPTION_TIMESTAMP_DESTINATIONORDER_QOS ? "by_reception_timestamp"
      : qos.kind == dds::BY_SOURCE_TIMESTAMP_DESTINATIONORDER_QOS ? "by_source_timestamp" : "unknown";
}

inline Descriptor Common(const auto& reliability, const auto& durability, const auto& liveliness,
                         const auto& ownership, const auto& deadline) {
  Descriptor result;
  auto& p = result.policies;
  p["dds_profile"] = IBMW_FASTDDS_V3_PLUS ? "fastdds3" : "fastdds2";
  p["reliability"] = reliability.kind == dds::RELIABLE_RELIABILITY_QOS ? "reliable"
      : reliability.kind == dds::BEST_EFFORT_RELIABILITY_QOS ? "best_effort" : "unknown";
  switch (durability.kind) {
    case dds::VOLATILE_DURABILITY_QOS: p["durability"] = "volatile"; break;
    case dds::TRANSIENT_LOCAL_DURABILITY_QOS: p["durability"] = "transient_local"; break;
    case dds::TRANSIENT_DURABILITY_QOS: p["durability"] = "transient"; break;
    case dds::PERSISTENT_DURABILITY_QOS: p["durability"] = "persistent"; break;
    default: p["durability"] = "unknown";
  }
  switch (liveliness.kind) {
    case dds::AUTOMATIC_LIVELINESS_QOS: p["liveliness"] = "automatic"; break;
    case dds::MANUAL_BY_PARTICIPANT_LIVELINESS_QOS: p["liveliness"] = "manual_by_participant"; break;
    case dds::MANUAL_BY_TOPIC_LIVELINESS_QOS: p["liveliness"] = "manual_by_topic"; break;
    default: p["liveliness"] = "unknown";
  }
  p["ownership"] = ownership.kind == dds::SHARED_OWNERSHIP_QOS ? "shared"
      : ownership.kind == dds::EXCLUSIVE_OWNERSHIP_QOS ? "exclusive" : "unknown";
  p["deadline"] = Duration(deadline.period);
  p["liveliness_lease_duration"] = Duration(liveliness.lease_duration);
  p["liveliness_announcement_period"] = Duration(liveliness.announcement_period);
  p["reliability_max_blocking_time"] = Duration(reliability.max_blocking_time);
  return result;
}

inline void Representation(Descriptor& result, const auto& policy) {
  std::vector<std::string> names;
  for (const auto value : policy.m_value) {
    names.emplace_back(value == dds::XCDR_DATA_REPRESENTATION ? "xcdr1"
        : value == dds::XCDR2_DATA_REPRESENTATION ? "xcdr2"
        : value == dds::XML_DATA_REPRESENTATION ? "xml" : "unknown");
  }
  result.policies["data_representation"] = nlohmann::json(names).dump();
}

inline void TypePolicy(Descriptor& result, const auto& policy) {
  auto& p = result.policies;
  p["type_consistency.kind"] = policy.m_kind == dds::DISALLOW_TYPE_COERCION ? "disallow" : "allow";
  p["type_consistency.force_type_validation"] = Boolean(policy.m_force_type_validation);
  p["type_consistency.ignore_sequence_bounds"] = Boolean(policy.m_ignore_sequence_bounds);
  p["type_consistency.ignore_string_bounds"] = Boolean(policy.m_ignore_string_bounds);
  p["type_consistency.ignore_member_names"] = Boolean(policy.m_ignore_member_names);
  p["type_consistency.prevent_type_widening"] = Boolean(policy.m_prevent_type_widening);
}

inline void Identifier(Descriptor& result, const std::string& name, const auto& id) {
  // Only equivalence hashes can be compared without resolving a TypeObject.
  if (id._d() != 0xf1 && id._d() != 0xf2) return;
  std::ostringstream stream;
  stream << std::hex << static_cast<unsigned>(id._d()) << ':';
  for (size_t i = 0; i < 14; ++i) stream << std::setw(2) << std::setfill('0') << static_cast<unsigned>(id.equivalence_hash()[i]);
  result.policies[name] = stream.str();
}

inline void TypeInformation(Descriptor& result, const auto& information) {
  const auto& complete = information.complete().typeid_with_size();
  const auto& minimal = information.minimal().typeid_with_size();
  Identifier(result, "type_complete", complete.type_id());
  Identifier(result, "type_minimal", minimal.type_id());
#if IBMW_FASTDDS_V3_PLUS
  if (result.policies.contains("type_complete")) result.policies["type_complete"] += ':' + std::to_string(complete.typeobject_serialized_size());
  if (result.policies.contains("type_minimal")) result.policies["type_minimal"] += ':' + std::to_string(minimal.typeobject_serialized_size());
#endif
}

// Captures the actual SEDP observation, including the absence of type metadata.
template <typename Endpoint>
Descriptor Discovery(const Endpoint& endpoint, bool publisher) {
#if IBMW_FASTDDS_V3_PLUS
  const auto& q = endpoint;
  auto result = Common(q.reliability, q.durability, q.liveliness, q.ownership, q.deadline);
  result.policies["presentation"] = Presentation(q.presentation);
  result.policies["destination_order"] = DestinationOrder(q.destination_order);
  result.policies["latency_budget"] = Duration(q.latency_budget.duration);
  if constexpr (requires { q.time_based_filter; }) result.policies["time_based_filter"] = Duration(q.time_based_filter.minimum_separation);
  if constexpr (requires { q.durability_service; }) result.policies["durability_service"] = DurabilityService(q.durability_service);
  result.policies["user_data"] = std::to_string(q.user_data.size()) + " bytes";
  result.policies["topic_data"] = std::to_string(q.topic_data.size()) + " bytes";
  result.policies["group_data"] = std::to_string(q.group_data.size()) + " bytes";
  result.policies["partition"] = nlohmann::json(q.partition.names()).dump();
  result.policies["disable_positive_acks"] = Boolean(q.disable_positive_acks.enabled);
  Representation(result, q.representation);
  if constexpr (requires { q.type_consistency; }) TypePolicy(result, q.type_consistency);
  if constexpr (requires { q.lifespan; }) if (publisher) result.policies["lifespan"] = Duration(q.lifespan.duration);
  if constexpr (requires { q.ownership_strength; }) if (publisher) result.policies["ownership_strength"] = std::to_string(q.ownership_strength.value);
  result.policies["topic_kind"] = endpoint.topic_kind == fastdds_compat::rtps::TopicKind_t::WITH_KEY ? "keyed" : "unkeyed";
  result.policies["type_information"] = "none";
  if (endpoint.type_information.assigned()) {
    result.policies["type_information"] = "present";
    TypeInformation(result, endpoint.type_information.type_information);
  }
#else
  const auto& q = endpoint.m_qos;
  auto result = Common(q.m_reliability, q.m_durability, q.m_liveliness, q.m_ownership, q.m_deadline);
  result.policies["presentation"] = Presentation(q.m_presentation);
  result.policies["destination_order"] = DestinationOrder(q.m_destinationOrder);
  result.policies["latency_budget"] = Duration(q.m_latencyBudget.duration);
  result.policies["time_based_filter"] = Duration(q.m_timeBasedFilter.minimum_separation);
  result.policies["durability_service"] = DurabilityService(q.m_durabilityService);
  result.policies["user_data"] = std::to_string(q.m_userData.size()) + " bytes";
  result.policies["topic_data"] = std::to_string(q.m_topicData.size()) + " bytes";
  result.policies["group_data"] = std::to_string(q.m_groupData.size()) + " bytes";
  result.policies["partition"] = nlohmann::json(q.m_partition.names()).dump();
  result.policies["disable_positive_acks"] = Boolean(q.m_disablePositiveACKs.enabled);
  Representation(result, q.representation);
  if constexpr (requires { q.type_consistency; }) TypePolicy(result, q.type_consistency);
  if (publisher) result.policies["lifespan"] = Duration(q.m_lifespan.duration);
  if constexpr (requires { q.m_ownershipStrength; }) if (publisher) result.policies["ownership_strength"] = std::to_string(q.m_ownershipStrength.value);
  result.policies["topic_kind"] = endpoint.topicKind() == fastdds_compat::rtps::TopicKind_t::WITH_KEY ? "keyed" : "unkeyed";
  result.policies["type_information"] = "none";
  if (endpoint.has_type_information() && endpoint.type_information().assigned()) {
    result.policies["type_information"] = "present";
    TypeInformation(result, endpoint.type_information().type_information);
  }
  if (endpoint.has_type_id() && endpoint.type_id().m_type_identifier._d() != 0) {
    result.policies["type_information"] = "present";
    Identifier(result, "type_identifier", endpoint.type_id().m_type_identifier);
  }
  if (endpoint.has_type() && endpoint.type().m_type_object._d() != 0) result.policies["type_information"] = "present";
#endif
  result.policies["qos_provenance"] = "discovered";
  return result;
}

// Entity-local policies are not in SEDP. Query the actual entity, not an XML/YAML template.
template <typename Qos>
Descriptor Local(const Qos& qos) {
  auto result = Common(qos.reliability(), qos.durability(), qos.liveliness(), qos.ownership(), qos.deadline());
  auto& p = result.policies;
  p["qos_provenance"] = "actual_entity";
  p["history"] = qos.history().kind == dds::KEEP_LAST_HISTORY_QOS ? "keep_last"
      : qos.history().kind == dds::KEEP_ALL_HISTORY_QOS ? "keep_all" : "unknown";
  p["depth"] = std::to_string(qos.history().depth);
  p["resource_limits.max_samples"] = std::to_string(qos.resource_limits().max_samples);
  p["resource_limits.max_instances"] = std::to_string(qos.resource_limits().max_instances);
  p["resource_limits.max_samples_per_instance"] = std::to_string(qos.resource_limits().max_samples_per_instance);
  p["destination_order"] = DestinationOrder(qos.destination_order());
  p["latency_budget"] = Duration(qos.latency_budget().duration);
  p["durability_service"] = DurabilityService(qos.durability_service());
  p["user_data"] = std::to_string(qos.user_data().size()) + " bytes";
  p["data_sharing"] = nlohmann::json{{"kind", static_cast<int>(qos.data_sharing().kind())},
                                    {"domain_ids", qos.data_sharing().domain_ids()}}.dump();
  if constexpr (requires { qos.transport_priority(); }) p["transport_priority"] = std::to_string(qos.transport_priority().value);
  if constexpr (requires { qos.time_based_filter(); }) p["time_based_filter"] = Duration(qos.time_based_filter().minimum_separation);
  if constexpr (requires { qos.reliable_writer_qos(); }) {
    p["disable_positive_acks"] = Boolean(qos.reliable_writer_qos().disable_positive_acks.enabled);
    p["disable_positive_acks_duration"] = Duration(qos.reliable_writer_qos().disable_positive_acks.duration);
    p["lifespan"] = Duration(qos.lifespan().duration);
    p["ownership_strength"] = std::to_string(qos.ownership_strength().value);
    p["publish_mode"] = std::to_string(static_cast<int>(qos.publish_mode().kind));
    p["writer_data_lifecycle"] = Boolean(qos.writer_data_lifecycle().autodispose_unregistered_instances);
    Representation(result, qos.representation());
  } else {
#if IBMW_FASTDDS_V3_PLUS
    p["disable_positive_acks"] = Boolean(qos.reliable_reader_qos().disable_positive_acks.enabled);
#else
    p["disable_positive_acks"] = Boolean(qos.reliable_reader_qos().disable_positive_ACKs.enabled);
#endif
    if constexpr (requires { qos.type_consistency().representation; }) {
      Representation(result, qos.type_consistency().representation);
      TypePolicy(result, qos.type_consistency().type_consistency);
    } else {
      Representation(result, qos.representation());
      TypePolicy(result, qos.type_consistency());
    }
    p["reader_data_lifecycle"] = Duration(qos.reader_data_lifecycle().autopurge_no_writer_samples_delay) + "; " +
                                 Duration(qos.reader_data_lifecycle().autopurge_disposed_samples_delay);
  }
  return result;
}

template <typename Entity, typename Parent, typename Topic>
Descriptor Actual(const Entity& entity, const Parent& parent, const Topic& topic) {
  auto result = Local(entity.get_qos());
  const auto* participant = parent.get_participant();
  const auto group = parent.get_qos();
  auto& p = result.policies;
  p["domain_id"] = std::to_string(participant->get_domain_id());
  p["partition"] = nlohmann::json(group.partition().names()).dump();
  p["presentation"] = Presentation(group.presentation());
  p["group_data"] = std::to_string(group.group_data().size()) + " bytes";
  p["entity_factory"] = Boolean(group.entity_factory().autoenable_created_entities);
  p["wire_topic_name"] = topic.get_name();
  p["wire_type_name"] = topic.get_type_name();
  p["topic_data"] = std::to_string(topic.get_qos().topic_data().size()) + " bytes";
  const auto type = parent.get_participant()->find_type(topic.get_type_name());
  if (type) {
#if IBMW_FASTDDS_V3_PLUS
    p["topic_kind"] = type->is_compute_key_provided ? "keyed" : "unkeyed";
#else
    p["topic_kind"] = type->m_isGetKeyDefined ? "keyed" : "unkeyed";
#endif
  }
  return result;
}
}  // namespace observation
}  // namespace ibmw::extensions::dds_extension::detail
