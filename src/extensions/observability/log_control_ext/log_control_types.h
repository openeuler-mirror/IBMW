// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ibmw::protocols::log_control_extension {

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
  if (j.contains("code")) j.at("code").get_to(v.code);
  if (j.contains("msg")) j.at("msg").get_to(v.msg);
  if (j.contains("module_log_level_map"))
    j.at("module_log_level_map").get_to(v.module_log_level_map);
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
  if (j.contains("code")) j.at("code").get_to(v.code);
  if (j.contains("msg")) j.at("msg").get_to(v.msg);
}

}  // namespace ibmw::protocols::log_control_extension
