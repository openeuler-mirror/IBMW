// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <limits>
#include <map>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ibmw::protocols::log_control_extension {

namespace detail {
// Validate before normalizing a response: absent fields must not become success.
template <typename Response>
Response ReadResponseStatus(const nlohmann::json& j) {
  if (!j.is_object()) throw std::invalid_argument("Expected a response object.");
  const auto& code = j.at("code");
  if (!code.is_number_integer() ||
      (!code.is_number_unsigned() && code.get<int64_t>() < 0) ||
      code.get<uint64_t>() > std::numeric_limits<decltype(Response{}.code)>::max())
    throw std::invalid_argument("Response code must be an unsigned integer in range.");
  Response result;
  code.get_to(result.code);
  j.at("msg").get_to(result.msg);
  return result;
}
}  // namespace detail

inline constexpr char kServiceName[] = "ibmw.protocols.log_control_extension.LogControlService";

// Strict, ASCII case-insensitive protocol validation. Do not use the config
// parser's unknown-name fallback to OFF for remote control requests.
inline std::optional<std::string_view> NormalizeLogLevel(std::string_view value) {
  for (std::string_view level : {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"}) {
    if (value.size() != level.size()) continue;
    bool equal = true;
    for (size_t i = 0; i < value.size(); ++i) {
      const char c = value[i] >= 'a' && value[i] <= 'z' ? value[i] - ('a' - 'A') : value[i];
      if (c != level[i]) { equal = false; break; }
    }
    if (equal) return level;
  }
  return std::nullopt;
}

/**
 * @brief Request for GetModuleLogLevel RPC
 *
 * Replaces protobuf message GetModuleLogLevelReq.
 * If module_names is empty, returns all modules.
 */
struct GetModuleLogLevelReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.log_control_extension.GetModuleLogLevelReq";

  std::vector<std::string> module_names;  // if empty, then get all modules
};

inline void to_json(nlohmann::json& j, const GetModuleLogLevelReq& v) {
  j = nlohmann::json{{"module_names", v.module_names}};
}

inline void from_json(const nlohmann::json& j, GetModuleLogLevelReq& v) {
  if (j.contains("module_names"))
    j.at("module_names").get_to(v.module_names);
}

/**
 * @brief Response for GetModuleLogLevel RPC
 */
struct GetModuleLogLevelRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.log_control_extension.GetModuleLogLevelRsp";

  uint32_t code = 0;
  std::string msg;
  std::map<std::string, std::string> module_log_level_map;  // key: module_name
};

inline void to_json(nlohmann::json& j, const GetModuleLogLevelRsp& v) {
  j = nlohmann::json{
      {"code", v.code},
      {"msg", v.msg},
      {"module_log_level_map", v.module_log_level_map}};
}

inline void from_json(const nlohmann::json& j, GetModuleLogLevelRsp& v) {
  auto result = detail::ReadResponseStatus<GetModuleLogLevelRsp>(j);
  if (result.code == 0 || j.contains("module_log_level_map")) {
    const auto& payload = j.at("module_log_level_map");
    if (!payload.is_object()) throw std::invalid_argument("Invalid response module_log_level_map.");
    payload.get_to(result.module_log_level_map);
  }
  v = std::move(result);
}

/**
 * @brief Request for SetModuleLogLevel RPC
 */
struct SetModuleLogLevelReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.log_control_extension.SetModuleLogLevelReq";

  std::map<std::string, std::string> module_log_level_map;
};

inline void to_json(nlohmann::json& j, const SetModuleLogLevelReq& v) {
  j = nlohmann::json{{"module_log_level_map", v.module_log_level_map}};
}

inline void from_json(const nlohmann::json& j, SetModuleLogLevelReq& v) {
  if (j.contains("module_log_level_map"))
    j.at("module_log_level_map").get_to(v.module_log_level_map);
}

/**
 * @brief Response for SetModuleLogLevel RPC
 */
struct SetModuleLogLevelRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.log_control_extension.SetModuleLogLevelRsp";

  uint32_t code = 0;
  std::string msg;
};

inline void to_json(nlohmann::json& j, const SetModuleLogLevelRsp& v) {
  j = nlohmann::json{{"code", v.code}, {"msg", v.msg}};
}

inline void from_json(const nlohmann::json& j, SetModuleLogLevelRsp& v) {
  auto result = detail::ReadResponseStatus<SetModuleLogLevelRsp>(j);
  v = std::move(result);
}

}  // namespace ibmw::protocols::log_control_extension
