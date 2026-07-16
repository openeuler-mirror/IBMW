// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// INTERNAL ONLY: generic ROS 2 Action DDS client-side observation helpers.
// This header is not a public API and must not be installed or included by
// generated, schema, template, or public API code.

#ifndef IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_CLIENT_OBSERVER_H_
#define IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_CLIENT_OBSERVER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "internal/ros2_action_dds/action_wire_contract.h"

namespace ibmw::internal::ros2_action_dds {

constexpr int8_t kActionClientGoalStatusUnknown = 0;

template <typename FeedbackT>
class ActionClientObserver {
 public:
  static constexpr std::size_t kObservationCap = 64;

  struct StatusSummary {
    bool available = false;
    std::size_t observed_arrays = 0;
    std::size_t matched_statuses = 0;
    int8_t latest_status = kActionClientGoalStatusUnknown;
    uint32_t observed_status_mask = 0;
    bool saw_terminal = false;
  };

  template <typename UuidT>
  void ResetActiveGoal(const UuidT& goal_id) {
    active_goal_key_ = UuidToString(goal_id);
    matched_feedback_ = false;
    latest_feedback_ = FeedbackT{};
    matched_status_ = false;
    observed_arrays_ = 0;
    matched_statuses_ = 0;
    latest_status_ = kActionClientGoalStatusUnknown;
    observed_status_mask_ = 0;
  }

  template <typename UuidT>
  bool ObserveFeedback(const UuidT& goal_id, const FeedbackT& feedback) {
    if (!GoalMatchesActive(goal_id)) {
      return false;
    }
    matched_feedback_ = true;
    latest_feedback_ = feedback;
    return true;
  }

  bool HasFeedback() const { return matched_feedback_; }

  const FeedbackT& LatestFeedback() const { return latest_feedback_; }

  template <typename StatusListT>
  void ObserveStatusList(const StatusListT& status_list) {
    observed_arrays_ = SaturatingIncrement(observed_arrays_, 1);

    std::size_t matching_statuses = 0;
    int8_t latest_status = kActionClientGoalStatusUnknown;
    uint32_t status_mask = 0;
    for (const auto& status : status_list) {
      if (GoalMatchesActive(status.goal_info.goal_id)) {
        ++matching_statuses;
        latest_status = status.status;
        status_mask |= StatusMaskBit(status.status);
      }
    }
    if (matching_statuses == 0) {
      return;
    }
    matched_status_ = true;
    matched_statuses_ = SaturatingIncrement(matched_statuses_, matching_statuses);
    latest_status_ = latest_status;
    observed_status_mask_ |= status_mask;
  }

  StatusSummary SummarizeTerminalStatus(int8_t terminal_status) const {
    StatusSummary summary;
    summary.available = matched_status_;
    summary.observed_arrays = observed_arrays_;
    summary.matched_statuses = matched_statuses_;
    summary.latest_status = latest_status_;
    summary.observed_status_mask = observed_status_mask_;
    summary.saw_terminal = (observed_status_mask_ & StatusMaskBit(terminal_status)) != 0;
    return summary;
  }

  static std::size_t SaturatingIncrement(std::size_t value, std::size_t increment) {
    if (value >= kObservationCap) {
      return kObservationCap;
    }
    const std::size_t remaining = kObservationCap - value;
    if (increment >= remaining) {
      return kObservationCap;
    }
    return value + increment;
  }

  static uint32_t StatusMaskBit(int8_t status) {
    if (status < 0 || status >= 32) {
      return 0;
    }
    return uint32_t{1} << static_cast<uint8_t>(status);
  }

 private:
  template <typename UuidT>
  bool GoalMatchesActive(const UuidT& goal_id) const {
    return !active_goal_key_.empty() && UuidToString(goal_id) == active_goal_key_;
  }

  std::string active_goal_key_;
  bool matched_feedback_ = false;
  FeedbackT latest_feedback_{};
  bool matched_status_ = false;
  std::size_t observed_arrays_ = 0;
  std::size_t matched_statuses_ = 0;
  int8_t latest_status_ = kActionClientGoalStatusUnknown;
  uint32_t observed_status_mask_ = 0;
};

}  // namespace ibmw::internal::ros2_action_dds

#endif  // IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_CLIENT_OBSERVER_H_
