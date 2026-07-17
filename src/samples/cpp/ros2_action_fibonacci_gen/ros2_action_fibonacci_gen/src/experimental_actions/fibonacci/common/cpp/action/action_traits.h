// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) trait contract.
//
// ibmw::action::ActionTraits<ActionT> is the single binding point between the
// public Action facade (ibmw::action::Server / ibmw::action::Client) and a
// concrete ROS 2 action type. The primary template is intentionally left
// undefined: a program that instantiates the facade for an action without a
// generated ActionTraits specialization fails to compile with a clear
// diagnostic (see IbmwActionTraitsSpecialized below) rather than an opaque
// template-instantiation error.
//
// A generated per-action header (emitted into the consuming project tree by
// ibmw-gen, never installed from ibmw) provides:
//   1. an ActionTraits<ActionT> specialization (this contract), and
//   2. the five action-specific Ros2FastTypeTraits<MsgT> specializations in
//      ibmw::internal::ros2_action_dds for the SendGoal / GetResult request and
//      response payloads plus the FeedbackMessage payload.
// The action-independent endpoints (CancelGoal request/response and
// GoalStatusArray) are already specialized once in the internal engine and must
// NOT be redeclared by generated code.

#ifndef IBMW_API_CPP_ACTION_ACTION_TRAITS_H_
#define IBMW_API_CPP_ACTION_ACTION_TRAITS_H_

#include <type_traits>

namespace ibmw::action {

// Primary template. Undefined on purpose. Specialized by generated per-action
// headers with the following required members:
//
//   // Message payload aliases (the eight action endpoints' types):
//   using Goal;               // <Action>::Goal
//   using Result;             // <Action>::Result
//   using Feedback;           // <Action>::Feedback
//   using SendGoalRequest;    // <Action>::Impl::SendGoalService::Request
//   using SendGoalResponse;   // <Action>::Impl::SendGoalService::Response
//   using GetResultRequest;   // <Action>::Impl::GetResultService::Request
//   using GetResultResponse;  // <Action>::Impl::GetResultService::Response
//   using FeedbackMessage;    // <Action>::Impl::FeedbackMessage
//   using GoalId;             // UUID message type (e.g. unique_identifier_msgs::msg::UUID)
//
//   // Canonical ROS 2 action type name (informational / diagnostics):
//   static constexpr const char* kActionTypeName;
//
//   // UUID accessors decouple the facade from field-name conventions:
//   static const GoalId& goal_id_of(const SendGoalRequest&);
//   static const GoalId& goal_id_of(const GetResultRequest&);
//   static const GoalId& feedback_goal_id(const FeedbackMessage&);
//
// Endpoint wire names and QoS are NOT part of this contract: they are derived
// at runtime from the action name by the engine (MakeEndpointNames + the
// Service/Feedback/Status QoS profiles), keeping ActionTraits types-only.
template <typename ActionT>
struct ActionTraits;

namespace detail {

template <typename ActionT, typename = void>
struct IsActionTraitsSpecialized : std::false_type {};

template <typename ActionT>
struct IsActionTraitsSpecialized<
    ActionT, std::void_t<typename ActionTraits<ActionT>::SendGoalRequest>>
    : std::true_type {};

}  // namespace detail

// Compile-time guard usable inside facade templates. Yields a readable error
// when a user instantiates Server/Client for an action whose generated
// ActionTraits specialization was not included.
template <typename ActionT>
inline constexpr bool kActionTraitsSpecialized =
    detail::IsActionTraitsSpecialized<ActionT>::value;

}  // namespace ibmw::action

#endif  // IBMW_API_CPP_ACTION_ACTION_TRAITS_H_
