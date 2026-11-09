// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <cstdint>
#include "nlohmann/json.hpp"

namespace ibmw::diag::fixture {
struct Message {
  static constexpr const char* kTypeName = "diag.Message";
  static constexpr const char* kJsonSchema =
      R"({"type":"object","required":["value"],"properties":{"value":{"type":"integer"}}})";
  int value = 0;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Message, value)
};
struct SumRequest {
  static constexpr const char* kTypeName = "diag.SumRequest";
  int64_t a = 0, b = 0;
  unsigned delay_ms = 0, status = 0;
};
inline void to_json(nlohmann::json& json, const SumRequest& value) {
  json = {{"a", value.a}, {"b", value.b}, {"delay_ms", value.delay_ms}, {"status", value.status}};
}
inline void from_json(const nlohmann::json& json, SumRequest& value) {
  value.a = json.at("a").get<int64_t>();
  value.b = json.at("b").get<int64_t>();
  value.delay_ms = json.value("delay_ms", 0u);
  value.status = json.value("status", 0u);
};
struct SumResponse {
  static constexpr const char* kTypeName = "diag.SumResponse";
  int64_t sum = 0;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(SumResponse, sum)
};
}  // namespace ibmw::diag::fixture
