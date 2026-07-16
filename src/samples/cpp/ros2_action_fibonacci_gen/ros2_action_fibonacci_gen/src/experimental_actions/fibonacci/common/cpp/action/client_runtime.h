// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) client runtime.
//
// ibmw::action::ClientRuntime<FeedbackPayload> is the generic, action-
// independent client-side observation engine shared by every DDS-only action
// client. It wraps the internal ActionClientObserver, forwarding feedback and
// status-array observation for the currently active goal. Action-specific
// feedback validation (e.g. domain-level prefix checks) lives in per-action
// wrappers, not here.

#ifndef IBMW_API_CPP_ACTION_CLIENT_RUNTIME_H_
#define IBMW_API_CPP_ACTION_CLIENT_RUNTIME_H_

#include <cstddef>
#include <cstdint>

#include "internal/ros2_action_dds/action_client_observer.h"

namespace ibmw::action {

inline constexpr int8_t kClientGoalStatusUnknown =
    ibmw::internal::ros2_action_dds::kActionClientGoalStatusUnknown;

template <typename FeedbackPayload>
class ClientRuntime {
 public:
  using Observer = ibmw::internal::ros2_action_dds::ActionClientObserver<FeedbackPayload>;
  using StatusSummary = typename Observer::StatusSummary;

  static constexpr std::size_t kObservationCap = Observer::kObservationCap;

  template <typename UuidT>
  void ResetActiveGoal(const UuidT& goal_id) {
    observer_.ResetActiveGoal(goal_id);
  }

  template <typename UuidT>
  bool ObserveFeedback(const UuidT& goal_id, const FeedbackPayload& feedback) {
    return observer_.ObserveFeedback(goal_id, feedback);
  }

  bool HasFeedback() const { return observer_.HasFeedback(); }

  const FeedbackPayload& LatestFeedback() const { return observer_.LatestFeedback(); }

  template <typename StatusListT>
  void ObserveStatusList(const StatusListT& status_list) {
    observer_.ObserveStatusList(status_list);
  }

  StatusSummary SummarizeTerminalStatus(int8_t terminal_status) const {
    return observer_.SummarizeTerminalStatus(terminal_status);
  }

  static std::size_t SaturatingIncrement(std::size_t value, std::size_t increment) {
    return Observer::SaturatingIncrement(value, increment);
  }

  static uint32_t StatusMaskBit(int8_t status) { return Observer::StatusMaskBit(status); }

 private:
  Observer observer_;
};

}  // namespace ibmw::action

#endif
