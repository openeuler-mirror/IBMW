// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Fibonacci ActionServerBinding specialization.
//
// This is the per-action wire adapter that connects the typed ROS 2 Fibonacci
// message structs (ActionTraits<example_interfaces::action::Fibonacci>) to the
// plain value payloads of FibonacciHook (ServerRuntime<FibonacciHook>). It is
// the single site that knows the Fibonacci goal/result/feedback field layout,
// keeping ActionTraits types-only and FibonacciHook free of ROS 2 wire
// dependencies. It is the third template parameter of ActionServerNode.
//
// Field mappings (mirrors the hand-written fibonacci_action_server.cpp):
//   GoalInputOf:        request.goal.order            -> int32_t
//   ToGetResultResponse: status                        -> response.status
//                        std::vector<int32_t> result   -> response.result.sequence
//   ToFeedbackMessage:  goal_id                        -> message.goal_id
//                        std::vector<int32_t> feedback  -> message.feedback.sequence

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_SERVER_BINDING_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_SERVER_BINDING_H_

#include <cstdint>

#include <example_interfaces/action/fibonacci.hpp>

#include "cpp/action/action_server_binding.h"
#include "fibonacci_action_server_runtime.h"
#include "fibonacci_action_traits.h"

namespace ibmw::action {

template <>
struct ActionServerBinding<ActionTraits<example_interfaces::action::Fibonacci>,
                           ibmw::samples::ros2_action_fibonacci::FibonacciHook> {
  using Traits = ActionTraits<example_interfaces::action::Fibonacci>;
  using Hook = ibmw::samples::ros2_action_fibonacci::FibonacciHook;

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
                                    ibmw::samples::ros2_action_fibonacci::FibonacciHook> = true;

}  // namespace ibmw::action

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_SERVER_BINDING_H_
