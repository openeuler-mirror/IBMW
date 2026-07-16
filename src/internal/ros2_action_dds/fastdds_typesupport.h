// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// INTERNAL ONLY: generic FastDDS TopicDataType machinery for ROS 2 Action DDS
// endpoints. This header is not a public API and must not be installed or
// included by generated, schema, template, or public API code except via the
// in-tree ibmw::internal include path.
//
// The per-message Ros2FastTypeTraits<MsgT> primary template is forward declared
// here. Action-independent endpoints (CancelGoal request/response and
// GoalStatusArray) are specialized in this header because their message types
// are identical for every ROS 2 Action. The five action-specific endpoints
// (SendGoal request/response, GetResult request/response, FeedbackMessage) are
// specialized by generated per-action headers that target this same primary
// template in this namespace.

#ifndef IBMW_INTERNAL_ROS2_ACTION_DDS_FASTDDS_TYPESUPPORT_H_
#define IBMW_INTERNAL_ROS2_ACTION_DDS_FASTDDS_TYPESUPPORT_H_

#include <cstdint>
#include <functional>

#include <action_msgs/msg/detail/goal_status_array__rosidl_typesupport_fastrtps_cpp.hpp>
#include <action_msgs/msg/goal_status_array.hpp>
#include <action_msgs/srv/cancel_goal.hpp>
#include <action_msgs/srv/detail/cancel_goal__rosidl_typesupport_fastrtps_cpp.hpp>
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "internal/ros2_action_dds/fastdds_wire_contract.h"
#if __has_include(<fastdds/rtps/common/InstanceHandle.hpp>)
#include <fastdds/rtps/common/InstanceHandle.hpp>
#else
#include <fastdds/rtps/common/InstanceHandle.h>
#endif
#if __has_include(<fastdds/rtps/common/SerializedPayload.hpp>)
#include <fastdds/rtps/common/SerializedPayload.hpp>
#else
#include <fastrtps/rtps/common/SerializedPayload.h>
#endif

namespace ibmw::internal::ros2_action_dds {

static constexpr auto kActionCdrType =
#if defined(FASTCDR_VERSION_MAJOR) && (FASTCDR_VERSION_MAJOR >= 2)
    eprosima::fastcdr::DDS_CDR;
#else
    eprosima::fastcdr::Cdr::DDS_CDR;
#endif

// Primary template. Specialized below for action-independent endpoints and by
// generated per-action headers for the five action-specific endpoints.
template <typename MsgT>
struct Ros2FastTypeTraits;

template <typename MsgT>
struct Ros2SerializedSizeProvider {
  void* data;

  uint32_t operator()() const {
    const auto& msg = *static_cast<const MsgT*>(data);
    return static_cast<uint32_t>(Ros2FastTypeTraits<MsgT>::Size(msg) + 4U);
  }
};

template <>
struct Ros2FastTypeTraits<action_msgs::srv::CancelGoal::Request> {
  static constexpr const char* kName = "action_msgs::srv::dds_::CancelGoal_Request_";
  static bool Serialize(const action_msgs::srv::CancelGoal::Request& msg, eprosima::fastcdr::Cdr& cdr) {
    return action_msgs::srv::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, action_msgs::srv::CancelGoal::Request& msg) {
    return action_msgs::srv::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const action_msgs::srv::CancelGoal::Request& msg) {
    return action_msgs::srv::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<action_msgs::srv::CancelGoal::Response> {
  static constexpr const char* kName = "action_msgs::srv::dds_::CancelGoal_Response_";
  static bool Serialize(const action_msgs::srv::CancelGoal::Response& msg, eprosima::fastcdr::Cdr& cdr) {
    return action_msgs::srv::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, action_msgs::srv::CancelGoal::Response& msg) {
    return action_msgs::srv::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const action_msgs::srv::CancelGoal::Response& msg) {
    return action_msgs::srv::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<action_msgs::msg::GoalStatusArray> {
  static constexpr const char* kName = "action_msgs::msg::dds_::GoalStatusArray_";
  static bool Serialize(const action_msgs::msg::GoalStatusArray& msg, eprosima::fastcdr::Cdr& cdr) {
    return action_msgs::msg::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, action_msgs::msg::GoalStatusArray& msg) {
    return action_msgs::msg::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const action_msgs::msg::GoalStatusArray& msg) {
    return action_msgs::msg::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <typename MsgT>
class Ros2FastTopicDataType final : public eprosima::fastdds::dds::TopicDataType {
 public:
  Ros2FastTopicDataType() {
#if IBMW_ACTION_HAS_MODERN_FASTDDS
    set_name(Ros2FastTypeTraits<MsgT>::kName);
    max_serialized_type_size = 65536;
    is_compute_key_provided = false;
#else
    setName(Ros2FastTypeTraits<MsgT>::kName);
    m_typeSize = 65536;
    m_isGetKeyDefined = false;
    auto_fill_type_object(false);
    auto_fill_type_information(false);
#endif
  }

#if IBMW_ACTION_HAS_MODERN_FASTDDS
  bool serialize(
      const void* const data,
      rtps_compat::SerializedPayload_t& payload,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
#else
  bool serialize(void* data, rtps_compat::SerializedPayload_t* payload_ptr) override {
    auto& payload = *payload_ptr;
#endif
    const auto& msg = *static_cast<const MsgT*>(data);
    const auto estimated_size = static_cast<uint32_t>(Ros2FastTypeTraits<MsgT>::Size(msg) + 4U);
    if (payload.max_size < estimated_size) {
      payload.reserve(estimated_size);
    }
    eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(payload.data), payload.max_size);
    eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kActionCdrType);
    cdr.serialize_encapsulation();
    if (!Ros2FastTypeTraits<MsgT>::Serialize(msg, cdr)) {
      return false;
    }
    payload.length = static_cast<uint32_t>(
#if defined(FASTCDR_VERSION_MAJOR) && (FASTCDR_VERSION_MAJOR >= 2)
        cdr.get_serialized_data_length());
#else
        cdr.getSerializedDataLength());
#endif
    payload.encapsulation = cdr.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE : CDR_LE;
    return true;
  }

#if IBMW_ACTION_HAS_MODERN_FASTDDS
  bool deserialize(rtps_compat::SerializedPayload_t& payload, void* data) override {
#else
  bool deserialize(rtps_compat::SerializedPayload_t* payload_ptr, void* data) override {
    auto& payload = *payload_ptr;
#endif
    auto& msg = *static_cast<MsgT*>(data);
    eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(payload.data), payload.length);
    eprosima::fastcdr::Cdr cdr(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kActionCdrType);
    cdr.read_encapsulation();
    return Ros2FastTypeTraits<MsgT>::Deserialize(cdr, msg);
  }

#if IBMW_ACTION_HAS_MODERN_FASTDDS
  uint32_t calculate_serialized_size(
      const void* const data,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
    const auto& msg = *static_cast<const MsgT*>(data);
    return static_cast<uint32_t>(Ros2FastTypeTraits<MsgT>::Size(msg) + 4U);
  }

  void* create_data() override { return new MsgT(); }
  void delete_data(void* data) override { delete static_cast<MsgT*>(data); }

  bool compute_key(
      rtps_compat::SerializedPayload_t& /*payload*/,
      rtps_compat::InstanceHandle_t& /*ihandle*/,
      bool /*force_md5*/ = false) override {
    return false;
  }

  bool compute_key(
      const void* const /*data*/,
      rtps_compat::InstanceHandle_t& /*ihandle*/,
      bool /*force_md5*/ = false) override {
    return false;
  }
#else
  std::function<uint32_t()> getSerializedSizeProvider(void* data) override {
    return Ros2SerializedSizeProvider<MsgT>{data};
  }

  void* createData() override { return new MsgT(); }
  void deleteData(void* data) override { delete static_cast<MsgT*>(data); }
  bool getKey(void*, rtps_compat::InstanceHandle_t*, bool = false) override { return false; }
#endif
};

template <typename MsgT>
eprosima::fastdds::dds::TypeSupport MakeTypeSupport() {
  return eprosima::fastdds::dds::TypeSupport(new Ros2FastTopicDataType<MsgT>());
}

}  // namespace ibmw::internal::ros2_action_dds

#endif  // IBMW_INTERNAL_ROS2_ACTION_DDS_FASTDDS_TYPESUPPORT_H_
