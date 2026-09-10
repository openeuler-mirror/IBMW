// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

namespace ibmw::examples::cpp::record_smoke_test {

struct JsonSmokeMessage {
  static constexpr const char* kTypeName = "ibmw.record_smoke.JsonSmokeMessage";
  static constexpr const char* kJsonSchema = R"({
    "$schema":"https://json-schema.org/draft/2020-12/schema",
    "type":"object",
    "required":["sequence","payload"],
    "properties":{
      "sequence":{"type":"integer","minimum":0},
      "payload":{"type":"string"}
    }
  })";

  uint64_t sequence = 0;
  std::string payload;
};

inline void to_json(nlohmann::json& value, const JsonSmokeMessage& message) {
  value = nlohmann::json{
      {"sequence", message.sequence}, {"payload", message.payload}};
}

inline void from_json(const nlohmann::json& value, JsonSmokeMessage& message) {
  value.at("sequence").get_to(message.sequence);
  value.at("payload").get_to(message.payload);
}

}  // namespace ibmw::examples::cpp::record_smoke_test
