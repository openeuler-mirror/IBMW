// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/service_discovery_protocol.h"

#include <charconv>
#include <random>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace ibmw::discovery::service_protocol {

std::string GrpcFrame(std::string_view payload) {
  if (payload.size() > kMaxSnapshotBytes) throw std::runtime_error("Service snapshot exceeds size limit.");
  std::string result(5, '\0');
  for (int i = 0; i < 4; ++i) result[1 + i] = static_cast<char>((payload.size() >> (24 - 8 * i)) & 255);
  result.append(payload);
  return result;
}

std::string_view GrpcPayload(std::string_view frame) {
  if (frame.size() < 5 || frame[0] != 0) throw std::runtime_error("Invalid service directory gRPC frame.");
  uint32_t size = 0;
  for (size_t i = 1; i < 5; ++i) size = (size << 8) | static_cast<unsigned char>(frame[i]);
  if (size > kMaxSnapshotBytes || size != frame.size() - 5)
    throw std::runtime_error("Invalid service directory gRPC payload length.");
  return frame.substr(5);
}

std::string NewInstanceId() {
  constexpr char digits[] = "0123456789abcdef";
  std::random_device random;
  std::string result;
  for (int i = 0; i < 20; ++i) result += digits[random() & 15];
  return result;
}

bool IsHidden(std::string_view name) {
  return name.starts_with('_') || name.find("/_") != std::string_view::npos;
}

std::string ZenohPrefix(std::string_view domain) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result = "ibmw/discovery/services/v1/d";
  for (const unsigned char byte : domain) {
    result += digits[byte >> 4]; result += digits[byte & 15];
  }
  return result + '/';
}

void Catalog::Add(std::string_view name, std::string_view request, std::string_view response) {
  if (name.empty() || request.empty() || response.empty())
    throw std::invalid_argument("Service discovery requires a name and both message types.");
  std::lock_guard lock(mutex_);
  const auto key = nlohmann::json::array({name, request, response}).dump();
  if (!services_.contains(key) && services_.size() >= kMaxServices)
    throw std::runtime_error("Too many service discovery entries.");
  services_.insert_or_assign(key, ibmw::extension::ServiceAdvertisement{
      .service_name = std::string(name), .request_type = std::string(request),
      .response_type = std::string(response), .hidden = IsHidden(name)});
}

std::string Catalog::Encode() const {
  std::lock_guard lock(mutex_);
  auto services = nlohmann::json::array();
  for (const auto& [key, value] : services_) {
    services.push_back({{"name", value.service_name}, {"request_type", value.request_type},
                        {"response_type", value.response_type}});
  }
  auto payload = nlohmann::json{{"version", 1}, {"instance", instance_},
                               {"services", std::move(services)}}.dump();
  if (payload.size() > kMaxSnapshotBytes) throw std::runtime_error("Service snapshot exceeds size limit.");
  return payload;
}

ServiceDiscoverySnapshot Decode(std::string_view payload, const ServiceDiscoverySource& source) {
  if (payload.size() > kMaxSnapshotBytes) throw std::runtime_error("Service snapshot exceeds size limit.");
  const auto json = nlohmann::json::parse(payload);
  if (!json.at("version").is_number_integer() || json.at("version") != 1)
    throw std::runtime_error("Unsupported service discovery version.");
  const auto instance = json.at("instance").get<std::string>();
  const auto& entries = json.at("services");
  if (instance.empty() || instance.size() > 256 || !entries.is_array() || entries.size() > kMaxServices)
    throw std::runtime_error("Invalid service discovery snapshot.");
  ServiceDiscoverySnapshot result;
  std::set<std::string> unique;
  for (const auto& entry : entries) {
    auto name = entry.at("name").get<std::string>();
    auto request = entry.at("request_type").get<std::string>();
    auto response = entry.at("response_type").get<std::string>();
    if (name.empty() || request.empty() || response.empty() ||
        name.find_first_of("\r\n") != std::string::npos ||
        request.find_first_of("\r\n") != std::string::npos ||
        response.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("Invalid service identity in discovery snapshot.");
    if (!unique.insert(nlohmann::json::array({name, request, response}).dump()).second) continue;
    result.services.push_back({.source = source, .runtime_id = instance,
        .advertisement = {.endpoint_id = std::to_string(result.services.size()),
            .service_name = name, .request_type = request, .response_type = response,
            .available_drivers = {std::string(BackendName(source.backend))}, .hidden = IsHidden(name)}});
  }
  result.sources.push_back({.source = source, .status = DiscoveryStatus::kReady});
  return result;
}

ServerAddress ParseServerAddress(std::string_view endpoint, std::string_view scheme) {
  const std::string prefix = std::string(scheme) + "://";
  if (endpoint.starts_with(prefix)) endpoint.remove_prefix(prefix.size());
  else if (endpoint.find("://") != std::string_view::npos)
    throw std::invalid_argument("Unsupported scheme; expected " + prefix + "host:port.");
  if (endpoint.ends_with('/')) endpoint.remove_suffix(1);
  if (endpoint.find_first_of("/@?#\r\n ") != std::string_view::npos)
    throw std::invalid_argument("Expected a server host:port without path or credentials.");
  ServerAddress result;
  if (endpoint.starts_with('[')) {
    const auto end = endpoint.find(']');
    if (end == std::string_view::npos || endpoint.substr(end + 1, 1) != ":")
      throw std::invalid_argument("Expected [IPv6]:port.");
    result.host = endpoint.substr(1, end - 1);
    result.port = endpoint.substr(end + 2);
  } else {
    const auto colon = endpoint.find(':');
    if (colon == std::string_view::npos || endpoint.find(':', colon + 1) != std::string_view::npos)
      throw std::invalid_argument("Expected host:port (bracket IPv6 addresses).");
    result.host = endpoint.substr(0, colon); result.port = endpoint.substr(colon + 1);
  }
  unsigned port = 0;
  const auto [last, error] = std::from_chars(result.port.data(), result.port.data() + result.port.size(), port);
  if (result.host.empty() || error != std::errc{} || last != result.port.data() + result.port.size() ||
      port == 0 || port > 65535)
    throw std::invalid_argument("Server port must be in [1, 65535].");
  return result;
}

}  // namespace ibmw::discovery::service_protocol
