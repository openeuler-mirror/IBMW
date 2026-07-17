// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// INTERNAL ONLY: FastDDS-specific ROS 2 Action DDS wire-contract helpers. This
// header is not a public API and must not be installed or included by generated,
// schema, template, or public API code.

#ifndef IBMW_INTERNAL_ROS2_ACTION_DDS_FASTDDS_WIRE_CONTRACT_H_
#define IBMW_INTERNAL_ROS2_ACTION_DDS_FASTDDS_WIRE_CONTRACT_H_

#include <cstdint>

#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#ifndef IBMW_ACTION_HAS_MODERN_FASTDDS
#if __has_include(<fastdds/config.hpp>)
#include <fastdds/config.hpp>
#define IBMW_ACTION_HAS_MODERN_FASTDDS 1
#else
#define IBMW_ACTION_HAS_MODERN_FASTDDS 0
#endif
#endif
#if __has_include(<fastdds/rtps/common/SampleIdentity.hpp>)
#include <fastdds/rtps/common/SampleIdentity.hpp>
#else
#include <fastdds/rtps/common/SampleIdentity.h>
#endif
#if __has_include(<fastdds/rtps/common/WriteParams.hpp>)
#include <fastdds/rtps/common/WriteParams.hpp>
#else
#include <fastdds/rtps/common/WriteParams.h>
#endif

#include "internal/ros2_action_dds/action_wire_contract.h"

namespace ibmw::internal::ros2_action_dds {

#if IBMW_ACTION_HAS_MODERN_FASTDDS
namespace rtps_compat = eprosima::fastdds::rtps;
#else
namespace rtps_compat = eprosima::fastrtps::rtps;
#endif

template <typename QosT>
void ApplyEndpointQosProfile(QosT& qos, const EndpointQosProfile& profile) {
  qos.reliability().kind = profile.reliable ? eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS
                                             : eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
  qos.durability().kind = profile.transient_local ? eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS
                                                  : eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
  qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
  qos.history().depth = profile.history_depth;
  qos.data_sharing().off();
}

inline void ApplyServiceEndpointQos(eprosima::fastdds::dds::DataReaderQos& qos) {
  ApplyEndpointQosProfile(qos, ServiceQosProfile());
}

inline void ApplyServiceEndpointQos(eprosima::fastdds::dds::DataWriterQos& qos) {
  ApplyEndpointQosProfile(qos, ServiceQosProfile());
}

inline void ApplyFeedbackEndpointQos(eprosima::fastdds::dds::DataReaderQos& qos) {
  ApplyEndpointQosProfile(qos, FeedbackQosProfile());
}

inline void ApplyFeedbackEndpointQos(eprosima::fastdds::dds::DataWriterQos& qos) {
  ApplyEndpointQosProfile(qos, FeedbackQosProfile());
}

inline void ApplyStatusEndpointQos(eprosima::fastdds::dds::DataReaderQos& qos) {
  ApplyEndpointQosProfile(qos, StatusQosProfile());
}

inline void ApplyStatusEndpointQos(eprosima::fastdds::dds::DataWriterQos& qos) {
  ApplyEndpointQosProfile(qos, StatusQosProfile());
}

inline rtps_compat::WriteParams MakeServiceRequestWriteParams(
    const rtps_compat::GUID_t& reply_reader_guid,
    uint32_t sequence) {
  rtps_compat::WriteParams params;
  params.related_sample_identity().writer_guid() = reply_reader_guid;
  params.related_sample_identity().sequence_number(rtps_compat::SequenceNumber_t(0, sequence));
  return params;
}

inline rtps_compat::SampleIdentity MakeCanonicalServiceRequestIdentity(
    const eprosima::fastdds::dds::SampleInfo& sample_info) {
  auto identity = sample_info.sample_identity;
  const auto& related_guid = sample_info.related_sample_identity.writer_guid();
  if (related_guid != rtps_compat::GUID_t::unknown()) {
    identity.writer_guid() = related_guid;
  }
  return identity;
}

inline rtps_compat::WriteParams MakeServiceResponseWriteParams(
    const rtps_compat::SampleIdentity& canonical_request_identity) {
  rtps_compat::WriteParams params;
  params.related_sample_identity(canonical_request_identity);
  return params;
}

inline bool ServiceReplyMatchesRequest(
    const rtps_compat::SampleIdentity& related_sample_identity,
    const rtps_compat::GUID_t& reply_reader_guid,
    uint32_t sequence) {
  return related_sample_identity.writer_guid() == reply_reader_guid &&
         related_sample_identity.sequence_number().low == sequence;
}

}  // namespace ibmw::internal::ros2_action_dds

#endif  // IBMW_INTERNAL_ROS2_ACTION_DDS_FASTDDS_WIRE_CONTRACT_H_
