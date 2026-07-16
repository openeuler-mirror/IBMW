// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "schema/extensions/common/header_types.h"

namespace ibmw::protocols::service_introspection {

/**
 * @brief Service event type
 */
enum class ServiceEventType : uint32_t {
  kClient = 0,  // Client
  kServer = 1,  // Server
};

NLOHMANN_JSON_SERIALIZE_ENUM(ServiceEventType, {
    {ServiceEventType::kClient, "CLIENT"},
    {ServiceEventType::kServer, "SERVER"},
})

/**
 * @brief Represents a single service call event
 *
 * Replaces the original RpcMsg protocol message.
 */
struct RpcMsg {
  static constexpr const char* kTypeName =
      "ibmw.protocols.service_introspection.RpcMsg";

  std::string service_name;
  ServiceEventType event_type = ServiceEventType::kClient;
  uint64_t time_stamp = 0;
  int64_t sequence_number = 0;
  uint32_t status_code = 0;
  std::string module_name;
  std::string driver_type;
  std::optional<std::string> request;
  std::optional<std::string> response;
};

inline void to_json(nlohmann::json& j, const RpcMsg& v) {
  j = nlohmann::json{
      {"service_name", v.service_name},
      {"event_type", v.event_type},
      {"time_stamp", v.time_stamp},
      {"sequence_number", v.sequence_number},
      {"status_code", v.status_code},
      {"module_name", v.module_name},
      {"driver_type", v.driver_type}};
  if (v.request.has_value()) j["request"] = v.request.value();
  if (v.response.has_value()) j["response"] = v.response.value();
}

inline void from_json(const nlohmann::json& j, RpcMsg& v) {
  if (j.contains("service_name")) j.at("service_name").get_to(v.service_name);
  if (j.contains("event_type")) j.at("event_type").get_to(v.event_type);
  if (j.contains("time_stamp")) j.at("time_stamp").get_to(v.time_stamp);
  if (j.contains("sequence_number")) j.at("sequence_number").get_to(v.sequence_number);
  if (j.contains("status_code")) j.at("status_code").get_to(v.status_code);
  if (j.contains("module_name")) j.at("module_name").get_to(v.module_name);
  if (j.contains("driver_type")) j.at("driver_type").get_to(v.driver_type);
  if (j.contains("request")) v.request = j.at("request").get<std::string>();
  if (j.contains("response")) v.response = j.at("response").get<std::string>();
}

/**
 * @brief Service call data frame
 *
 * Replaces the original RpcMsgList protocol message.
 */
struct RpcMsgList {
  static constexpr const char* kTypeName =
      "ibmw.protocols.service_introspection.RpcMsgList";

  ibmw::protocols::common::Header header;
  std::vector<RpcMsg> rpc_msg;
};

inline void to_json(nlohmann::json& j, const RpcMsgList& v) {
  j = nlohmann::json{
      {"header", v.header},
      {"rpc_msg", v.rpc_msg}};
}

inline void from_json(const nlohmann::json& j, RpcMsgList& v) {
  if (j.contains("header")) j.at("header").get_to(v.header);
  if (j.contains("rpc_msg")) j.at("rpc_msg").get_to(v.rpc_msg);
}

}  // namespace ibmw::protocols::service_introspection
