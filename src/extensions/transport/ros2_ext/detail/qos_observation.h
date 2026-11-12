// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <string>
#include <rmw/qos_string_conversions.h>
#include <rmw/types.h>
#include "cpp/transport/topic_descriptor.h"

namespace ibmw::extensions::ros2_extension::detail {
inline std::string DescribeDuration(const rmw_time_t& duration) {
  constexpr rmw_time_t infinite = RMW_DURATION_INFINITE;
  if (duration.sec == infinite.sec && duration.nsec == infinite.nsec) return "infinite";
  if (duration.sec == 0 && duration.nsec == 0) return "default";
  return std::to_string(duration.sec) + " s " + std::to_string(duration.nsec) + " ns";
}

inline ibmw::transport::QosDescriptor DescribeQos(const rmw_qos_profile_t& qos) {
  ibmw::transport::QosDescriptor result;
  const auto add = [&](const char* name, const char* value) {
    if (value) result.policies[name] = value;
  };
  add("reliability", rmw_qos_reliability_policy_to_str(qos.reliability));
  add("durability", rmw_qos_durability_policy_to_str(qos.durability));
  add("liveliness", rmw_qos_liveliness_policy_to_str(qos.liveliness));
  add("history", rmw_qos_history_policy_to_str(qos.history));
  result.policies["deadline"] = DescribeDuration(qos.deadline);
  result.policies["lifespan"] = DescribeDuration(qos.lifespan);
  result.policies["liveliness_lease_duration"] = DescribeDuration(qos.liveliness_lease_duration);
  if (qos.history == RMW_QOS_POLICY_HISTORY_KEEP_LAST) {
    result.policies["depth"] = std::to_string(qos.depth);
  }
  result.policies["avoid_ros_namespace_conventions"] = qos.avoid_ros_namespace_conventions ? "true" : "false";
  return result;
}

}  // namespace ibmw::extensions::ros2_extension::detail
