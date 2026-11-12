// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once
#include "cpp/util/json_type_support.h"
#include "type_support/text_serialization.h"
namespace ibmw::cli::service::test {
struct Request {
  static constexpr const char* kTypeName = "service_call.Request";
  int64_t a = 0, b = 0;
  uint32_t delay_ms = 0, status = 0;
};
struct Response {
  static constexpr const char* kTypeName = "service_call.Response";
  int64_t sum = 0;
};
inline void from_json(const nlohmann::json& j, Request& r) {
  ibmw::util::text::Object(j, {"a", "b", "delay_ms", "status"});
  ibmw::util::text::Field(j, "a", r.a); ibmw::util::text::Field(j, "b", r.b);
  ibmw::util::text::Field(j, "delay_ms", r.delay_ms); ibmw::util::text::Field(j, "status", r.status);
}
inline void to_json(nlohmann::json& j, const Request& r) { j = {{"a", r.a}, {"b", r.b}, {"delay_ms", r.delay_ms}, {"status", r.status}}; }
inline void from_json(const nlohmann::json& j, Response& r) { ibmw::util::text::Object(j, {"sum"}); ibmw::util::text::Field(j, "sum", r.sum); }
inline void to_json(nlohmann::json& j, const Response& r) { j = {{"sum", r.sum}}; }
}
