// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace ibmw::protocols::time_manipulator_extension {

// --- CommonRsp (shared response for all 3 RPCs) ---

struct CommonRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.time_manipulator_extension.CommonRsp";

  double time_ratio = 0.0;
  uint32_t code = 0;
  std::string msg;
};

inline void to_json(nlohmann::json& j, const CommonRsp& v) {
  j = nlohmann::json{
      {"time_ratio", v.time_ratio},
      {"code", v.code},
      {"msg", v.msg}};
}

inline void from_json(const nlohmann::json& j, CommonRsp& v) {
  if (j.contains("time_ratio")) j.at("time_ratio").get_to(v.time_ratio);
  if (j.contains("code")) j.at("code").get_to(v.code);
  if (j.contains("msg")) j.at("msg").get_to(v.msg);
}

// --- SetTimeRatio ---

struct SetTimeRatioReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.time_manipulator_extension.SetTimeRatioReq";

  std::string executor_name;
  double time_ratio = 0.0;
};

inline void to_json(nlohmann::json& j, const SetTimeRatioReq& v) {
  j = nlohmann::json{
      {"executor_name", v.executor_name},
      {"time_ratio", v.time_ratio}};
}

inline void from_json(const nlohmann::json& j, SetTimeRatioReq& v) {
  if (j.contains("executor_name")) j.at("executor_name").get_to(v.executor_name);
  if (j.contains("time_ratio")) j.at("time_ratio").get_to(v.time_ratio);
}

// --- Pause ---

struct PauseReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.time_manipulator_extension.PauseReq";

  std::string executor_name;
};

inline void to_json(nlohmann::json& j, const PauseReq& v) {
  j = nlohmann::json{{"executor_name", v.executor_name}};
}

inline void from_json(const nlohmann::json& j, PauseReq& v) {
  if (j.contains("executor_name")) j.at("executor_name").get_to(v.executor_name);
}

// --- GetTimeRatio ---

struct GetTimeRatioReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.time_manipulator_extension.GetTimeRatioReq";

  std::string executor_name;
};

inline void to_json(nlohmann::json& j, const GetTimeRatioReq& v) {
  j = nlohmann::json{{"executor_name", v.executor_name}};
}

inline void from_json(const nlohmann::json& j, GetTimeRatioReq& v) {
  if (j.contains("executor_name")) j.at("executor_name").get_to(v.executor_name);
}

}  // namespace ibmw::protocols::time_manipulator_extension
