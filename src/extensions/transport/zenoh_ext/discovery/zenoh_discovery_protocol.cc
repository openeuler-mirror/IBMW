// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_discovery_protocol.h"

#include <memory>
#include <set>
#include <stdexcept>
#include <json/json.h>
#include <zenoh.h>

namespace ibmw::discovery::zenoh_protocol {
namespace {
std::string ReadString(const Json::Value& value, const char* name, bool required = false) {
  if (!value[name].isString() || (required && value[name].asString().empty())) {
    throw std::runtime_error(std::string("Invalid Zenoh discovery field: ") + name);
  }
  return value[name].asString();
}
}  // namespace

std::string DomainPrefix(std::string_view domain) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "ibmw/discovery/v1/d";
  for (unsigned char byte : domain) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result + '/';
}

std::string SnapshotKey(std::string_view domain, std::string_view instance) {
  if (instance.empty() || instance.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
    throw std::runtime_error("Invalid Zenoh discovery instance ID.");
  }
  return DomainPrefix(domain) + std::string(instance) + "/snapshot";
}

bool IsConcreteKey(std::string_view key) noexcept {
  return !key.empty() && key.find('\0') == std::string_view::npos &&
         key.find('*') == std::string_view::npos &&
         z_keyexpr_is_canon(key.data(), key.size()) == Z_OK;
}

std::string Encode(const Snapshot& snapshot) {
  if (snapshot.endpoints.size() > kMaxEndpoints) throw std::runtime_error("Too many Zenoh discovery endpoints.");
  Json::Value root(Json::objectValue);
  root["version"] = 1;
  root["instance"] = snapshot.instance;
  root["domain"] = snapshot.domain;
  root["revision"] = Json::UInt64(snapshot.revision);
  root["endpoints"] = Json::Value(Json::arrayValue);
  for (const auto& endpoint : snapshot.endpoints) {
    Json::Value item(Json::objectValue);
    item["id"] = endpoint.endpoint_id;
    item["role"] = endpoint.role == EndpointRole::kPublisher ? "publisher" : "subscriber";
    item["topic"] = endpoint.topic_name;
    item["type"] = endpoint.type_name;
    item["key"] = endpoint.wire_topic_name;
    item["serialization"] = endpoint.serialization_type;
    item["qos"] = Json::Value(Json::objectValue);
    for (const auto& [name, value] : endpoint.qos.policies) item["qos"][name] = value;
    root["endpoints"].append(std::move(item));
  }
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  auto result = Json::writeString(writer, root);
  if (result.size() > kMaxSnapshotBytes) throw std::runtime_error("Zenoh discovery snapshot exceeds size limit.");
  return result;
}

Snapshot Decode(std::string_view payload, std::string_view reply_key, const DiscoverySource& source) {
  if (payload.empty() || payload.size() > kMaxSnapshotBytes) throw std::runtime_error("Invalid Zenoh discovery snapshot size.");
  Json::CharReaderBuilder builder;
  builder["rejectDupKeys"] = true;
  builder["failIfExtra"] = true;
  builder["stackLimit"] = 32;
  Json::Value root;
  std::string error;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(payload.data(), payload.data() + payload.size(), &root, &error) ||
      !root.isObject() || !root["version"].isUInt() || root["version"].asUInt() != 1 ||
      !root["revision"].isUInt64() || !root["endpoints"].isArray() ||
      root["endpoints"].size() > kMaxEndpoints) {
    throw std::runtime_error("Invalid or unsupported Zenoh discovery snapshot.");
  }
  Snapshot result{.instance = ReadString(root, "instance", true),
                  .domain = ReadString(root, "domain"), .revision = root["revision"].asUInt64()};
  if (result.domain != source.domain || reply_key != SnapshotKey(result.domain, result.instance)) {
    throw std::runtime_error("Zenoh discovery reply key or domain does not match its snapshot.");
  }
  std::set<std::string> ids;
  for (const auto& item : root["endpoints"]) {
    if (!item.isObject()) throw std::runtime_error("Invalid Zenoh discovery endpoint.");
    DiscoveredEndpoint endpoint{.source = source, .participant_id = result.instance,
        .endpoint_id = ReadString(item, "id", true)};
    if (!ids.insert(endpoint.endpoint_id).second) throw std::runtime_error("Duplicate Zenoh endpoint ID.");
    const auto role = ReadString(item, "role");
    if (role != "publisher" && role != "subscriber") throw std::runtime_error("Invalid Zenoh endpoint role.");
    endpoint.role = role == "publisher" ? EndpointRole::kPublisher : EndpointRole::kSubscriber;
    endpoint.topic_name = ReadString(item, "topic", true);
    endpoint.type_name = ReadString(item, "type", true);
    endpoint.wire_topic_name = ReadString(item, "key", true);
    if (!IsConcreteKey(endpoint.wire_topic_name)) {
      throw std::runtime_error("Zenoh discovery requires a concrete canonical key without NUL or wildcards.");
    }
    endpoint.serialization_type = ReadString(item, "serialization");
    if (!item["qos"].isObject()) throw std::runtime_error("Invalid Zenoh endpoint QoS.");
    for (const auto& name : item["qos"].getMemberNames()) endpoint.qos.policies[name] = ReadString(item["qos"], name.c_str());
    endpoint.available_drivers = {"zenoh"};
    endpoint.hidden = endpoint.topic_name.starts_with('_') || endpoint.topic_name.find("/_") != std::string::npos;
    result.endpoints.push_back(std::move(endpoint));
  }
  return result;
}
}  // namespace ibmw::discovery::zenoh_protocol
