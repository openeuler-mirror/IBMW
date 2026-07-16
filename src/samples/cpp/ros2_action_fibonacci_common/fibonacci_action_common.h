// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_COMMON_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_COMMON_H_

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "internal/ros2_action_dds/action_wire_contract.h"

namespace ibmw::samples::ros2_action_fibonacci {

constexpr int32_t kMaxFibonacciOrder = 47;
constexpr std::size_t kDefaultMaxConcurrentGoals = 4;
constexpr std::size_t kDefaultMaxRetainedGoals = 32;
constexpr std::size_t kDefaultMaxPendingResultsPerGoal = 16;

using EndpointNames = ibmw::internal::ros2_action_dds::EndpointNames;
using EndpointQosProfile = ibmw::internal::ros2_action_dds::EndpointQosProfile;

inline std::string StripOneLeadingSlash(std::string name) {
  return ibmw::internal::ros2_action_dds::StripOneLeadingSlash(std::move(name));
}

inline EndpointNames MakeEndpointNames(const std::string& action_name) {
  return ibmw::internal::ros2_action_dds::MakeEndpointNames(action_name);
}

inline EndpointQosProfile ServiceQosProfile() {
  return ibmw::internal::ros2_action_dds::ServiceQosProfile();
}

inline EndpointQosProfile FeedbackQosProfile() {
  return ibmw::internal::ros2_action_dds::FeedbackQosProfile();
}

inline EndpointQosProfile StatusQosProfile() {
  return ibmw::internal::ros2_action_dds::StatusQosProfile();
}

inline std::vector<int32_t> BuildFibonacciSequence(int32_t order) {
  std::vector<int32_t> sequence;
  if (order <= 0) {
    return sequence;
  }
  sequence.reserve(static_cast<std::size_t>(order));
  sequence.push_back(0);
  if (order > 1) {
    sequence.push_back(1);
  }
  for (int32_t index = 2; index < order; ++index) {
    sequence.push_back(sequence[static_cast<std::size_t>(index - 1)] +
                       sequence[static_cast<std::size_t>(index - 2)]);
  }
  return sequence;
}

inline std::string JoinSequence(const std::vector<int32_t>& sequence) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < sequence.size(); ++i) {
    if (i != 0U) {
      oss << ", ";
    }
    oss << sequence[i];
  }
  oss << "]";
  return oss.str();
}

inline bool IsValidGoalOrder(int32_t order) {
  return order > 0 && order <= kMaxFibonacciOrder;
}

inline bool CanAcceptGoal(std::size_t active_goals, std::size_t retained_goals,
                          std::size_t max_active = kDefaultMaxConcurrentGoals,
                          std::size_t max_retained = kDefaultMaxRetainedGoals) {
  return active_goals < max_active && retained_goals < max_retained;
}

template <typename UuidT>
bool IsZeroUuid(const UuidT& uuid) {
  return ibmw::internal::ros2_action_dds::IsZeroUuid(uuid);
}

template <typename UuidT>
std::string UuidToString(const UuidT& uuid) {
  return ibmw::internal::ros2_action_dds::UuidToString(uuid);
}

template <typename UuidT, typename TimeT>
bool IsCancelAllActiveRequest(const UuidT& goal_id, const TimeT& stamp) {
  return ibmw::internal::ros2_action_dds::IsCancelAllActiveRequest(goal_id, stamp);
}

template <typename ClockT = std::chrono::steady_clock>
bool ResultRetentionExpired(typename ClockT::time_point terminal_time,
                            typename ClockT::time_point now,
                            std::chrono::seconds retention) {
  return ibmw::internal::ros2_action_dds::ResultRetentionExpired<ClockT>(terminal_time, now, retention);
}

}  // namespace ibmw::samples::ros2_action_fibonacci

#endif  // IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_COMMON_H_
