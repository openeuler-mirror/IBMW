// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_FASTDDS_TYPESUPPORT_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_FASTDDS_TYPESUPPORT_H_

#include <cstddef>

#include <control_msgs/action/detail/gripper_command__rosidl_typesupport_fastrtps_cpp.hpp>
#include <control_msgs/action/gripper_command.hpp>
#include <fastcdr/Cdr.h>

#include "internal/ros2_action_dds/fastdds_typesupport.h"

namespace ibmw::internal::ros2_action_dds {

using GripperCommand = control_msgs::action::GripperCommand;

template <>
struct Ros2FastTypeTraits<GripperCommand::Impl::SendGoalService::Request> {
  static constexpr const char* kName = "control_msgs::action::dds_::GripperCommand_SendGoal_Request_";
  static bool Serialize(const GripperCommand::Impl::SendGoalService::Request& msg,
                        eprosima::fastcdr::Cdr& cdr) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr,
                          GripperCommand::Impl::SendGoalService::Request& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const GripperCommand::Impl::SendGoalService::Request& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<GripperCommand::Impl::SendGoalService::Response> {
  static constexpr const char* kName = "control_msgs::action::dds_::GripperCommand_SendGoal_Response_";
  static bool Serialize(const GripperCommand::Impl::SendGoalService::Response& msg,
                        eprosima::fastcdr::Cdr& cdr) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr,
                          GripperCommand::Impl::SendGoalService::Response& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const GripperCommand::Impl::SendGoalService::Response& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<GripperCommand::Impl::GetResultService::Request> {
  static constexpr const char* kName = "control_msgs::action::dds_::GripperCommand_GetResult_Request_";
  static bool Serialize(const GripperCommand::Impl::GetResultService::Request& msg,
                        eprosima::fastcdr::Cdr& cdr) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr,
                          GripperCommand::Impl::GetResultService::Request& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const GripperCommand::Impl::GetResultService::Request& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<GripperCommand::Impl::GetResultService::Response> {
  static constexpr const char* kName = "control_msgs::action::dds_::GripperCommand_GetResult_Response_";
  static bool Serialize(const GripperCommand::Impl::GetResultService::Response& msg,
                        eprosima::fastcdr::Cdr& cdr) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr,
                          GripperCommand::Impl::GetResultService::Response& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const GripperCommand::Impl::GetResultService::Response& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

template <>
struct Ros2FastTypeTraits<GripperCommand::Impl::FeedbackMessage> {
  static constexpr const char* kName = "control_msgs::action::dds_::GripperCommand_FeedbackMessage_";
  static bool Serialize(const GripperCommand::Impl::FeedbackMessage& msg,
                        eprosima::fastcdr::Cdr& cdr) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);
  }
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr,
                          GripperCommand::Impl::FeedbackMessage& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::cdr_deserialize(cdr, msg);
  }
  static size_t Size(const GripperCommand::Impl::FeedbackMessage& msg) {
    return control_msgs::action::typesupport_fastrtps_cpp::get_serialized_size(msg, 0);
  }
};

}  // namespace ibmw::internal::ros2_action_dds

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_FASTDDS_TYPESUPPORT_H_
