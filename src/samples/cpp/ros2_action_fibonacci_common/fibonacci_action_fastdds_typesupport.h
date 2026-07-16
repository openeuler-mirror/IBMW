// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_FASTDDS_TYPESUPPORT_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_FASTDDS_TYPESUPPORT_H_

#include <cstddef>

#include <example_interfaces/action/detail/fibonacci__rosidl_typesupport_fastrtps_cpp.hpp>
#include <example_interfaces/action/fibonacci.hpp>
#include <fastcdr/Cdr.h>

#include "internal/ros2_action_dds/fastdds_typesupport.h"

namespace ibmw::internal::ros2_action_dds {

using Fibonacci = example_interfaces::action::Fibonacci;

template <>
struct Ros2FastTypeTraits<Fibonacci::Impl::SendGoalService::Request> {
  static constexpr const char* kName = "example_interfaces::action::dds_::Fibonacci_SendGoal_Request_";
  static bool Serialize(const Fibonacci::Impl::SendGoalService::Request& msg, eprosima::fastcdr::Cdr& cdr) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, Fibonacci::Impl::SendGoalService::Request& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const Fibonacci::Impl::SendGoalService::Request& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<Fibonacci::Impl::SendGoalService::Response> {
  static constexpr const char* kName = "example_interfaces::action::dds_::Fibonacci_SendGoal_Response_";
  static bool Serialize(const Fibonacci::Impl::SendGoalService::Response& msg, eprosima::fastcdr::Cdr& cdr) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, Fibonacci::Impl::SendGoalService::Response& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const Fibonacci::Impl::SendGoalService::Response& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<Fibonacci::Impl::GetResultService::Request> {
  static constexpr const char* kName = "example_interfaces::action::dds_::Fibonacci_GetResult_Request_";
  static bool Serialize(const Fibonacci::Impl::GetResultService::Request& msg, eprosima::fastcdr::Cdr& cdr) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, Fibonacci::Impl::GetResultService::Request& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const Fibonacci::Impl::GetResultService::Request& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<Fibonacci::Impl::GetResultService::Response> {
  static constexpr const char* kName = "example_interfaces::action::dds_::Fibonacci_GetResult_Response_";
  static bool Serialize(const Fibonacci::Impl::GetResultService::Response& msg, eprosima::fastcdr::Cdr& cdr) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, Fibonacci::Impl::GetResultService::Response& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const Fibonacci::Impl::GetResultService::Response& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<Fibonacci::Impl::FeedbackMessage> {
  static constexpr const char* kName = "example_interfaces::action::dds_::Fibonacci_FeedbackMessage_";
  static bool Serialize(const Fibonacci::Impl::FeedbackMessage& msg, eprosima::fastcdr::Cdr& cdr) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, Fibonacci::Impl::FeedbackMessage& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const Fibonacci::Impl::FeedbackMessage& msg) {
    return example_interfaces::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

}  // namespace ibmw::internal::ros2_action_dds

namespace ibmw::samples::ros2_action_fibonacci {

using Fibonacci = example_interfaces::action::Fibonacci;
namespace action_rtps = ibmw::internal::ros2_action_dds::rtps_compat;

using ibmw::internal::ros2_action_dds::kActionCdrType;
using ibmw::internal::ros2_action_dds::MakeTypeSupport;
using ibmw::internal::ros2_action_dds::Ros2FastTopicDataType;
using ibmw::internal::ros2_action_dds::Ros2FastTypeTraits;

}  // namespace ibmw::samples::ros2_action_fibonacci

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_FASTDDS_TYPESUPPORT_H_
