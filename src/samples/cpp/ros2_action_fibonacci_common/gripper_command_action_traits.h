// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_TRAITS_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_TRAITS_H_

#include <control_msgs/action/gripper_command.hpp>
#include <unique_identifier_msgs/msg/uuid.hpp>

#include "cpp/action/action_traits.h"
#include "gripper_command_action_fastdds_typesupport.h"

namespace ibmw::action {

template <>
struct ActionTraits<control_msgs::action::GripperCommand> {
  using Action = control_msgs::action::GripperCommand;
  using Goal = Action::Goal;
  using Result = Action::Result;
  using Feedback = Action::Feedback;
  using SendGoalRequest = Action::Impl::SendGoalService::Request;
  using SendGoalResponse = Action::Impl::SendGoalService::Response;
  using GetResultRequest = Action::Impl::GetResultService::Request;
  using GetResultResponse = Action::Impl::GetResultService::Response;
  using FeedbackMessage = Action::Impl::FeedbackMessage;
  using GoalId = unique_identifier_msgs::msg::UUID;

  static constexpr const char* kActionTypeName = "control_msgs/action/GripperCommand";

  static const GoalId& goal_id_of(const SendGoalRequest& request) { return request.goal_id; }
  static const GoalId& goal_id_of(const GetResultRequest& request) { return request.goal_id; }
  static const GoalId& feedback_goal_id(const FeedbackMessage& message) { return message.goal_id; }
};

}  // namespace ibmw::action

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_TRAITS_H_
