// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_CLIENT_RUNTIME_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_CLIENT_RUNTIME_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cpp/action/client_runtime.h"
#include "fibonacci_action_common.h"

namespace ibmw::samples::ros2_action_fibonacci {

constexpr int8_t kClientGoalStatusUnknown = ibmw::action::kClientGoalStatusUnknown;

class FibonacciActionClientRuntime {
 public:
  using Runtime = ibmw::action::ClientRuntime<std::vector<int32_t>>;
  using StatusSummary = Runtime::StatusSummary;

  static constexpr std::size_t kObservationCap = Runtime::kObservationCap;

  struct FeedbackValidation {
    bool available = false;
    bool ok = true;
    std::vector<int32_t> sequence;
  };

  template <typename UuidT>
  void ResetActiveGoal(const UuidT& goal_id) {
    runtime_.ResetActiveGoal(goal_id);
  }

  template <typename UuidT>
  bool ObserveFeedback(const UuidT& goal_id, const std::vector<int32_t>& sequence) {
    return runtime_.ObserveFeedback(goal_id, sequence);
  }

  FeedbackValidation ValidateFeedbackPrefix(const std::vector<int32_t>& expected) const {
    FeedbackValidation validation;
    validation.available = runtime_.HasFeedback();
    validation.sequence = runtime_.LatestFeedback();
    if (!validation.available) {
      return validation;
    }
    validation.ok = validation.sequence.size() <= expected.size() &&
                    std::equal(validation.sequence.begin(), validation.sequence.end(), expected.begin());
    return validation;
  }

  template <typename StatusListT>
  void ObserveStatusList(const StatusListT& status_list) {
    runtime_.ObserveStatusList(status_list);
  }

  StatusSummary SummarizeTerminalStatus(int8_t terminal_status) const {
    return runtime_.SummarizeTerminalStatus(terminal_status);
  }

  static std::size_t SaturatingIncrement(std::size_t value, std::size_t increment) {
    return Runtime::SaturatingIncrement(value, increment);
  }

  static uint32_t StatusMaskBit(int8_t status) { return Runtime::StatusMaskBit(status); }

 private:
  Runtime runtime_;
};

}  // namespace ibmw::samples::ros2_action_fibonacci

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_CLIENT_RUNTIME_H_
