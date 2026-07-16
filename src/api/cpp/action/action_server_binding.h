// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) server wire binding.
//
// ibmw::action::ActionServerBinding<Traits, Hook> is the per-action adapter
// between ROS 2 wire message structs (typed by ActionTraits<Action>) and the
// plain value payloads of the Hook lifecycle policy (ServerRuntime<Hook>). It
// is ActionServerNode's third template parameter and the only site that knows a
// specific action's goal/result/feedback field layout, keeping ActionTraits
// types-only and Hook free of ROS 2 wire dependencies.
//
// The primary template is intentionally undefined. Each action supplies an
// explicit specialization with three static members:
//
//   template <>
//   struct ActionServerBinding<ActionTraits<MyAction>, MyHook> {
//     using Traits = ActionTraits<MyAction>;
//     using Hook = MyHook;
//     static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest&);
//     static Traits::GetResultResponse ToGetResultResponse(
//         int8_t status, const Hook::ResultPayload&);
//     static Traits::FeedbackMessage ToFeedbackMessage(
//         const Traits::GoalId&, const Hook::FeedbackPayload&);
//   };
//
// A specialization must also set kActionServerBindingSpecialized<Traits, Hook>
// to true; ActionServerNode static_asserts on it so a missing binding fails
// with a clear diagnostic rather than an incomplete-type error.

#ifndef IBMW_API_CPP_ACTION_ACTION_SERVER_BINDING_H_
#define IBMW_API_CPP_ACTION_ACTION_SERVER_BINDING_H_

namespace ibmw::action {

template <typename Traits, typename Hook>
struct ActionServerBinding;

template <typename Traits, typename Hook>
inline constexpr bool kActionServerBindingSpecialized = false;

}  // namespace ibmw::action

#endif  // IBMW_API_CPP_ACTION_ACTION_SERVER_BINDING_H_
