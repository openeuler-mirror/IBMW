// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_FASTDDS_HELPERS_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_FASTDDS_HELPERS_H_

#include "fibonacci_action_common.h"
#include "internal/ros2_action_dds/fastdds_wire_contract.h"

namespace ibmw::samples::ros2_action_fibonacci {

namespace action_rtps = ibmw::internal::ros2_action_dds::rtps_compat;

template <typename QosT>
void ApplyEndpointQosProfile(QosT& qos, const EndpointQosProfile& profile) {
  ibmw::internal::ros2_action_dds::ApplyEndpointQosProfile(qos, profile);
}

inline void ApplyServiceEndpointQos(eprosima::fastdds::dds::DataReaderQos& qos) {
  ibmw::internal::ros2_action_dds::ApplyServiceEndpointQos(qos);
}

inline void ApplyServiceEndpointQos(eprosima::fastdds::dds::DataWriterQos& qos) {
  ibmw::internal::ros2_action_dds::ApplyServiceEndpointQos(qos);
}

inline void ApplyFeedbackEndpointQos(eprosima::fastdds::dds::DataReaderQos& qos) {
  ibmw::internal::ros2_action_dds::ApplyFeedbackEndpointQos(qos);
}

inline void ApplyFeedbackEndpointQos(eprosima::fastdds::dds::DataWriterQos& qos) {
  ibmw::internal::ros2_action_dds::ApplyFeedbackEndpointQos(qos);
}

inline void ApplyStatusEndpointQos(eprosima::fastdds::dds::DataReaderQos& qos) {
  ibmw::internal::ros2_action_dds::ApplyStatusEndpointQos(qos);
}

inline void ApplyStatusEndpointQos(eprosima::fastdds::dds::DataWriterQos& qos) {
  ibmw::internal::ros2_action_dds::ApplyStatusEndpointQos(qos);
}

inline action_rtps::WriteParams MakeServiceRequestWriteParams(
    const action_rtps::GUID_t& reply_reader_guid,
    uint32_t sequence) {
  return ibmw::internal::ros2_action_dds::MakeServiceRequestWriteParams(reply_reader_guid, sequence);
}

inline action_rtps::SampleIdentity MakeCanonicalServiceRequestIdentity(
    const eprosima::fastdds::dds::SampleInfo& sample_info) {
  return ibmw::internal::ros2_action_dds::MakeCanonicalServiceRequestIdentity(sample_info);
}

inline action_rtps::WriteParams MakeServiceResponseWriteParams(
    const action_rtps::SampleIdentity& canonical_request_identity) {
  return ibmw::internal::ros2_action_dds::MakeServiceResponseWriteParams(canonical_request_identity);
}

inline bool ServiceReplyMatchesRequest(
    const action_rtps::SampleIdentity& related_sample_identity,
    const action_rtps::GUID_t& reply_reader_guid,
    uint32_t sequence) {
  return ibmw::internal::ros2_action_dds::ServiceReplyMatchesRequest(
      related_sample_identity, reply_reader_guid, sequence);
}

}  // namespace ibmw::samples::ros2_action_fibonacci

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_FASTDDS_HELPERS_H_
