// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_COUNTDOWN_ACTION_SERVER_BINDING_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_COUNTDOWN_ACTION_SERVER_BINDING_H_

#include <cstdint>
#include <vector>

#include <example_interfaces/action/fibonacci.hpp>

#include "cpp/action/action_server_binding.h"
#include "cpp/action/server_runtime.h"
#include "fibonacci_action_common.h"
#include "fibonacci_action_traits.h"

namespace ibmw::samples::ros2_action_fibonacci {

struct CountdownHook {
  using GoalInput = int32_t;
  using FeedbackPayload = std::vector<int32_t>;
  using ResultPayload = std::vector<int32_t>;

  struct ExecState {
    int32_t current = 0;
    std::vector<int32_t> sequence;
  };

  static bool ValidateGoal(const GoalInput& count) { return IsValidGoalOrder(count); }

  static ExecState InitState(const GoalInput& count) {
    ExecState state;
    state.current = count;
    state.sequence.push_back(count);
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    if (state.current <= 0) {
      return ibmw::action::TickOutcome::kSucceeded;
    }
    --state.current;
    state.sequence.push_back(state.current);
    return state.current == 0 ? ibmw::action::TickOutcome::kSucceeded
                              : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) { return state.sequence; }
  static ResultPayload BuildResult(const ExecState& state) { return state.sequence; }
};

}  // namespace ibmw::samples::ros2_action_fibonacci

namespace ibmw::action {

template <>
struct ActionServerBinding<ActionTraits<example_interfaces::action::Fibonacci>,
                           ibmw::samples::ros2_action_fibonacci::CountdownHook> {
  using Traits = ActionTraits<example_interfaces::action::Fibonacci>;
  using Hook = ibmw::samples::ros2_action_fibonacci::CountdownHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return request.goal.order;
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload& result) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.sequence = result;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.sequence = feedback;
    return message;
  }
};

template <>
inline constexpr bool
    kActionServerBindingSpecialized<ActionTraits<example_interfaces::action::Fibonacci>,
                                    ibmw::samples::ros2_action_fibonacci::CountdownHook> = true;

}  // namespace ibmw::action

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_COUNTDOWN_ACTION_SERVER_BINDING_H_
