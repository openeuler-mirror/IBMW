// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// INTERNAL ONLY: generic ROS 2 Action DDS wire-contract helpers. This header
// is not a public API and must not be installed or included by generated,
// schema, template, or public API code.

#ifndef IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_WIRE_CONTRACT_H_
#define IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_WIRE_CONTRACT_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace ibmw::internal::ros2_action_dds {

struct EndpointNames {
  std::string send_goal_request;
  std::string send_goal_reply;
  std::string get_result_request;
  std::string get_result_reply;
  std::string cancel_goal_request;
  std::string cancel_goal_reply;
  std::string feedback;
  std::string status;
};

struct EndpointQosProfile {
  int history_depth = 10;
  bool reliable = true;
  bool transient_local = false;
};

inline std::string StripOneLeadingSlash(std::string name) {
  if (!name.empty() && name.front() == '/') {
    name.erase(name.begin());
  }
  return name;
}

inline EndpointNames MakeEndpointNames(const std::string& action_name) {
  const std::string action = StripOneLeadingSlash(action_name);
  EndpointNames names;
  names.send_goal_request = "rq/" + action + "/_action/send_goalRequest";
  names.send_goal_reply = "rr/" + action + "/_action/send_goalReply";
  names.get_result_request = "rq/" + action + "/_action/get_resultRequest";
  names.get_result_reply = "rr/" + action + "/_action/get_resultReply";
  names.cancel_goal_request = "rq/" + action + "/_action/cancel_goalRequest";
  names.cancel_goal_reply = "rr/" + action + "/_action/cancel_goalReply";
  names.feedback = "rt/" + action + "/_action/feedback";
  names.status = "rt/" + action + "/_action/status";
  return names;
}

inline EndpointQosProfile ServiceQosProfile() {
  return EndpointQosProfile{.history_depth = 10, .reliable = true, .transient_local = false};
}

inline EndpointQosProfile FeedbackQosProfile() {
  return EndpointQosProfile{.history_depth = 10, .reliable = true, .transient_local = false};
}

inline EndpointQosProfile StatusQosProfile() {
  return EndpointQosProfile{.history_depth = 1, .reliable = true, .transient_local = true};
}

template <typename UuidT>
bool IsZeroUuid(const UuidT& uuid) {
  return std::all_of(uuid.uuid.begin(), uuid.uuid.end(), [](uint8_t value) { return value == 0U; });
}

template <typename UuidT>
std::string UuidToString(const UuidT& uuid) {
  return std::string(reinterpret_cast<const char*>(uuid.uuid.data()), uuid.uuid.size());
}

template <typename UuidT, typename TimeT>
bool IsCancelAllActiveRequest(const UuidT& goal_id, const TimeT& stamp) {
  return IsZeroUuid(goal_id) && stamp.sec == 0 && stamp.nanosec == 0;
}

template <typename ClockT = std::chrono::steady_clock>
bool ResultRetentionExpired(typename ClockT::time_point terminal_time,
                            typename ClockT::time_point now,
                            std::chrono::seconds retention) {
  return terminal_time + retention < now;
}

}  // namespace ibmw::internal::ros2_action_dds

#endif  // IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_WIRE_CONTRACT_H_
