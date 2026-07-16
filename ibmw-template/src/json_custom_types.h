// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// JSON custom message types for pub/sub and RPC demos.
// No IDL or code generation required — just plain C++ structs
// with nlohmann::json serialization support.
//
// Uses NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT to auto-generate
// to_json / from_json — each field is listed only ONCE.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace my_project {

// ---------------------------------------------------------------------------
// Pub/Sub message: SensorReading
// ---------------------------------------------------------------------------

struct SensorReading {
  static constexpr const char* kTypeName = "my_project.SensorReading";

  std::string sensor_id;
  double temperature = 0.0;
  double humidity = 0.0;
  uint64_t timestamp_ms = 0;
  std::vector<double> extra_values;

  // Auto-generates to_json / from_json — fields listed once only.
  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
      SensorReading, sensor_id, temperature, humidity, timestamp_ms, extra_values)
};

// ---------------------------------------------------------------------------
// RPC request / response types: ComputeRequest, ComputeResponse
// ---------------------------------------------------------------------------

struct ComputeRequest {
  static constexpr const char* kTypeName = "my_project.ComputeRequest";

  std::string operation;   // e.g. "add", "multiply"
  double operand_a = 0.0;
  double operand_b = 0.0;

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
      ComputeRequest, operation, operand_a, operand_b)
};

struct ComputeResponse {
  static constexpr const char* kTypeName = "my_project.ComputeResponse";

  uint32_t code = 0;
  std::string msg;
  double result = 0.0;

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
      ComputeResponse, code, msg, result)
};

}  // namespace my_project
