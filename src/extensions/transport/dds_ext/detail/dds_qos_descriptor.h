// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include <fastdds/dds/core/policy/QosPolicies.hpp>

#include "engine/transport/topic_descriptor.h"

namespace ibmw::extensions::dds_extension::detail {

inline std::optional<int32_t> ParsePositiveDepth(std::string_view value) {
  int32_t depth = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), depth);
  if (result.ec != std::errc() ||
      result.ptr != value.data() + value.size() || depth <= 0) {
    return std::nullopt;
  }
  return depth;
}

template <typename Qos>
void ApplyQosDescriptor(
    const runtime::core::transport::QosDescriptor& descriptor,
    Qos& qos) {
  const auto history = descriptor.policies.find("history");
  if (history != descriptor.policies.end()) {
    if (history->second == "keep_all") {
      qos.history().kind = eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS;
    } else if (history->second == "keep_last") {
      qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    }
  }

  const auto depth = descriptor.policies.find("depth");
  if (depth != descriptor.policies.end() &&
      qos.history().kind == eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS) {
    if (const auto parsed_depth = ParsePositiveDepth(depth->second)) {
      qos.history().depth = *parsed_depth;
      if (qos.resource_limits().max_samples_per_instance >= 0 &&
          qos.resource_limits().max_samples_per_instance < *parsed_depth) {
        qos.resource_limits().max_samples_per_instance = *parsed_depth;
      }
      if (qos.resource_limits().max_samples >= 0 &&
          qos.resource_limits().max_samples < *parsed_depth) {
        qos.resource_limits().max_samples = *parsed_depth;
      }
    }
  }

  const auto reliability = descriptor.policies.find("reliability");
  if (reliability != descriptor.policies.end()) {
    if (reliability->second == "reliable") {
      qos.reliability().kind =
          eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
    } else if (reliability->second == "best_effort") {
      qos.reliability().kind =
          eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
    }
  }

  const auto durability = descriptor.policies.find("durability");
  if (durability != descriptor.policies.end()) {
    if (durability->second == "transient_local") {
      qos.durability().kind =
          eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;
    } else if (durability->second == "volatile") {
      qos.durability().kind =
          eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
    }
  }
}

}  // namespace ibmw::extensions::dds_extension::detail
