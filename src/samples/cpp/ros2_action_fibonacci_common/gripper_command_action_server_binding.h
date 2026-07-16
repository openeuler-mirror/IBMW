// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_SERVER_BINDING_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_SERVER_BINDING_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <control_msgs/action/gripper_command.hpp>

#include "cpp/action/action_server_binding.h"
#include "cpp/action/server_runtime.h"
#include "gripper_command_action_traits.h"

namespace ibmw::samples::ros2_action_fibonacci {

struct GripperCommandPayload {
  double position = 0.0;
  double effort = 0.0;
  bool stalled = false;
  bool reached_goal = false;
};

struct GripperCommandHook {
  using GoalInput = GripperCommandPayload;
  using FeedbackPayload = GripperCommandPayload;
  using ResultPayload = GripperCommandPayload;

  struct ExecState {
    double target_position = 0.0;
    double max_effort = 0.0;
    double current_position = 0.0;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput& goal) {
    return std::isfinite(goal.position) && std::isfinite(goal.effort) && goal.position >= 0.0 &&
           goal.position <= 1.0 && goal.effort >= 0.0;
  }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.target_position = goal.position;
    state.max_effort = goal.effort;
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    const double progress = std::min(1.0, static_cast<double>(state.step) / 5.0);
    state.current_position = state.target_position * progress;
    return state.step >= 5 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    return GripperCommandPayload{.position = state.current_position,
                                 .effort = state.max_effort,
                                 .stalled = false,
                                 .reached_goal = state.step >= 5};
  }

  static ResultPayload BuildResult(const ExecState& state) {
    return GripperCommandPayload{.position = state.target_position,
                                 .effort = state.max_effort,
                                 .stalled = false,
                                 .reached_goal = true};
  }
};

}  // namespace ibmw::samples::ros2_action_fibonacci

namespace ibmw::action {

template <>
struct ActionServerBinding<ActionTraits<control_msgs::action::GripperCommand>,
                           ibmw::samples::ros2_action_fibonacci::GripperCommandHook> {
  using Traits = ActionTraits<control_msgs::action::GripperCommand>;
  using Hook = ibmw::samples::ros2_action_fibonacci::GripperCommandHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.position = request.goal.command.position,
                           .effort = request.goal.command.max_effort,
                           .stalled = false,
                           .reached_goal = false};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload& result) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.position = result.position;
    response.result.effort = result.effort;
    response.result.stalled = result.stalled;
    response.result.reached_goal = result.reached_goal;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.position = feedback.position;
    message.feedback.effort = feedback.effort;
    message.feedback.stalled = feedback.stalled;
    message.feedback.reached_goal = feedback.reached_goal;
    return message;
  }
};

template <>
inline constexpr bool
    kActionServerBindingSpecialized<ActionTraits<control_msgs::action::GripperCommand>,
                                    ibmw::samples::ros2_action_fibonacci::GripperCommandHook> = true;

}  // namespace ibmw::action

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_GRIPPER_COMMAND_ACTION_SERVER_BINDING_H_
