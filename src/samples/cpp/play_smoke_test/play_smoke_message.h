// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

namespace ibmw::examples::cpp::play_smoke_test {

struct JsonHeader {
  uint64_t time_stamp = 0;
  std::string frame_id;
  uint64_t sequence_num = 0;
};

inline void to_json(nlohmann::json& value, const JsonHeader& message) {
  value = nlohmann::json{{"time_stamp", message.time_stamp},
                         {"frame_id", message.frame_id},
                         {"sequence_num", message.sequence_num}};
}

inline void from_json(const nlohmann::json& value, JsonHeader& message) {
  value.at("time_stamp").get_to(message.time_stamp);
  value.at("frame_id").get_to(message.frame_id);
  value.at("sequence_num").get_to(message.sequence_num);
}

}  // namespace ibmw::examples::cpp::play_smoke_test
