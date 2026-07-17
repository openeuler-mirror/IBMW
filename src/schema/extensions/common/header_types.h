// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace ibmw::protocols::common {

/**
 * @brief Common message header with timestamp and metadata
 *
 * Replaces the original Header protocol message.
 */
struct Header {
  static constexpr const char* kTypeName = "ibmw.protocols.common.Header";

  uint64_t time_stamp = 0;
  std::string frame_id;
  uint64_t sequence_num = 0;
};

inline void to_json(nlohmann::json& j, const Header& v) {
  j = nlohmann::json{
      {"time_stamp", v.time_stamp},
      {"frame_id", v.frame_id},
      {"sequence_num", v.sequence_num}};
}

inline void from_json(const nlohmann::json& j, Header& v) {
  if (j.contains("time_stamp")) j.at("time_stamp").get_to(v.time_stamp);
  if (j.contains("frame_id")) j.at("frame_id").get_to(v.frame_id);
  if (j.contains("sequence_num")) j.at("sequence_num").get_to(v.sequence_num);
}

}  // namespace ibmw::protocols::common
