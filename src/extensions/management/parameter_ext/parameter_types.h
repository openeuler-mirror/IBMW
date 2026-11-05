// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ibmw::protocols::parameter_extension {

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

inline constexpr char kServiceName[] = "ibmw.protocols.parameter_extension.ParameterService";

struct ListModulesReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.ListModulesReq";
};
inline void to_json(nlohmann::json& j, const ListModulesReq&) {
  j = nlohmann::json::object();
}
inline void from_json(const nlohmann::json& j, ListModulesReq&) {
  if (!j.is_object()) throw std::invalid_argument("Expected a ListModules request object.");
}

struct ListModulesRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.ListModulesRsp";
  uint32_t code = 0;
  std::string msg;
  std::vector<std::string> module_names;
};
inline void to_json(nlohmann::json& j, const ListModulesRsp& v) {
  j = nlohmann::json{{"code", v.code}, {"msg", v.msg}, {"module_names", v.module_names}};
}
inline void from_json(const nlohmann::json& j, ListModulesRsp& v) {
  auto result = detail::ReadResponseStatus<ListModulesRsp>(j);
  if (result.code == 0 || j.contains("module_names")) {
    const auto& payload = j.at("module_names");
    if (!payload.is_array()) throw std::invalid_argument("Invalid response module_names.");
    payload.get_to(result.module_names);
  }
  v = std::move(result);
}

// --- SetParameter ---

struct SetParameterReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.SetParameterReq";

  std::string module_name;
  std::string parameter_key;
  std::string parameter_value;
};

inline void to_json(nlohmann::json& j, const SetParameterReq& v) {
  j = nlohmann::json{
      {"module_name", v.module_name},
      {"parameter_key", v.parameter_key},
      {"parameter_value", v.parameter_value}};
}

inline void from_json(const nlohmann::json& j, SetParameterReq& v) {
  if (j.contains("module_name")) j.at("module_name").get_to(v.module_name);
  if (j.contains("parameter_key")) j.at("parameter_key").get_to(v.parameter_key);
  if (j.contains("parameter_value")) j.at("parameter_value").get_to(v.parameter_value);
}

struct SetParameterRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.SetParameterRsp";

  uint64_t code = 0;
  std::string msg;
};

inline void to_json(nlohmann::json& j, const SetParameterRsp& v) {
  j = nlohmann::json{{"code", v.code}, {"msg", v.msg}};
}

inline void from_json(const nlohmann::json& j, SetParameterRsp& v) {
  auto result = detail::ReadResponseStatus<SetParameterRsp>(j);
  v = std::move(result);
}

// --- GetParameter ---

struct GetParameterReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.GetParameterReq";

  std::string module_name;
  std::string parameter_key;
};

inline void to_json(nlohmann::json& j, const GetParameterReq& v) {
  j = nlohmann::json{
      {"module_name", v.module_name},
      {"parameter_key", v.parameter_key}};
}

inline void from_json(const nlohmann::json& j, GetParameterReq& v) {
  if (j.contains("module_name")) j.at("module_name").get_to(v.module_name);
  if (j.contains("parameter_key")) j.at("parameter_key").get_to(v.parameter_key);
}

struct GetParameterRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.GetParameterRsp";

  uint32_t code = 0;
  std::string msg;
  std::string parameter_value;
};

inline void to_json(nlohmann::json& j, const GetParameterRsp& v) {
  j = nlohmann::json{
      {"code", v.code},
      {"msg", v.msg},
      {"parameter_value", v.parameter_value}};
}

inline void from_json(const nlohmann::json& j, GetParameterRsp& v) {
  auto result = detail::ReadResponseStatus<GetParameterRsp>(j);
  if (result.code == 0 || j.contains("parameter_value")) {
    const auto& payload = j.at("parameter_value");
    if (!payload.is_string()) throw std::invalid_argument("Invalid response parameter_value.");
    payload.get_to(result.parameter_value);
  }
  v = std::move(result);
}

// --- ListParameter ---

struct ListParameterReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.ListParameterReq";

  std::string module_name;
};

inline void to_json(nlohmann::json& j, const ListParameterReq& v) {
  j = nlohmann::json{{"module_name", v.module_name}};
}

inline void from_json(const nlohmann::json& j, ListParameterReq& v) {
  if (j.contains("module_name")) j.at("module_name").get_to(v.module_name);
}

struct ListParameterRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.ListParameterRsp";

  uint32_t code = 0;
  std::string msg;
  std::vector<std::string> parameter_keys;
};

inline void to_json(nlohmann::json& j, const ListParameterRsp& v) {
  j = nlohmann::json{
      {"code", v.code},
      {"msg", v.msg},
      {"parameter_keys", v.parameter_keys}};
}

inline void from_json(const nlohmann::json& j, ListParameterRsp& v) {
  auto result = detail::ReadResponseStatus<ListParameterRsp>(j);
  if (result.code == 0 || j.contains("parameter_keys")) {
    const auto& payload = j.at("parameter_keys");
    if (!payload.is_array()) throw std::invalid_argument("Invalid response parameter_keys.");
    payload.get_to(result.parameter_keys);
  }
  v = std::move(result);
}

// --- ParameterMap (nested map type used by Dump/Load) ---

struct ParameterMap {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.ParameterMap";

  std::map<std::string, std::string> value;
};

inline void to_json(nlohmann::json& j, const ParameterMap& v) {
  j = nlohmann::json{{"value", v.value}};
}

inline void from_json(const nlohmann::json& j, ParameterMap& v) {
  if (j.contains("value")) j.at("value").get_to(v.value);
}

// --- DumpParameter ---

struct DumpParameterReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.DumpParameterReq";

  std::vector<std::string> module_names;
};

inline void to_json(nlohmann::json& j, const DumpParameterReq& v) {
  j = nlohmann::json{{"module_names", v.module_names}};
}

inline void from_json(const nlohmann::json& j, DumpParameterReq& v) {
  if (j.contains("module_names")) j.at("module_names").get_to(v.module_names);
}

struct DumpParameterRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.DumpParameterRsp";

  uint32_t code = 0;
  std::string msg;
  std::map<std::string, ParameterMap> module_parameter_map;  // key: module_name
};

inline void to_json(nlohmann::json& j, const DumpParameterRsp& v) {
  j = nlohmann::json{
      {"code", v.code},
      {"msg", v.msg},
      {"module_parameter_map", v.module_parameter_map}};
}

inline void from_json(const nlohmann::json& j, DumpParameterRsp& v) {
  if (j.contains("code")) j.at("code").get_to(v.code);
  if (j.contains("msg")) j.at("msg").get_to(v.msg);
  if (j.contains("module_parameter_map"))
    j.at("module_parameter_map").get_to(v.module_parameter_map);
}

// --- LoadParameter ---

struct LoadParameterReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.LoadParameterReq";

  std::map<std::string, ParameterMap> module_parameter_map;  // key: module_name
};

inline void to_json(nlohmann::json& j, const LoadParameterReq& v) {
  j = nlohmann::json{{"module_parameter_map", v.module_parameter_map}};
}

inline void from_json(const nlohmann::json& j, LoadParameterReq& v) {
  if (j.contains("module_parameter_map"))
    j.at("module_parameter_map").get_to(v.module_parameter_map);
}

struct LoadParameterRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.parameter_extension.LoadParameterRsp";

  uint32_t code = 0;
  std::string msg;
};

inline void to_json(nlohmann::json& j, const LoadParameterRsp& v) {
  j = nlohmann::json{{"code", v.code}, {"msg", v.msg}};
}

inline void from_json(const nlohmann::json& j, LoadParameterRsp& v) {
  if (j.contains("code")) j.at("code").get_to(v.code);
  if (j.contains("msg")) j.at("msg").get_to(v.msg);
}

}  // namespace ibmw::protocols::parameter_extension
