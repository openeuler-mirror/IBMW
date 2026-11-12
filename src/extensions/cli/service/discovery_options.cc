// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_options.h"

#include <stdexcept>
#include <string>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <utility>

#include "extension_api/ibmw_runtime_directory.h"

namespace ibmw::cli::service {

namespace {
std::optional<std::string> Read(const Invocation& invocation, const std::string& name) {
  const auto found = invocation.options.find(name);
  if (found == invocation.options.end()) return std::nullopt;
  if (found->second.size() != 1 || found->second.front().empty())
    throw std::invalid_argument(name + " requires exactly one nonempty value.");
  return found->second.front();
}
uint32_t Domain(const std::string& value) {
  uint32_t result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("Domain must be a nonnegative decimal integer.");
  return result;
}

ibmw::discovery::ServiceDiscoveryTlsOptions ParseDiscoveryTlsOptions(const Invocation& invocation) {
  // Values are read before each aggregate is built: GCC before 13 (PR66139)
  // leaks the members already constructed when an aggregate initializer throws.
  auto truststore = Read(invocation, "--mqtt-truststore").value_or("");
  auto client_cert = Read(invocation, "--mqtt-client-cert").value_or("");
  auto client_key = Read(invocation, "--mqtt-client-key").value_or("");
  ibmw::discovery::ServiceDiscoveryTlsOptions result{.truststore = std::move(truststore),
                                                     .client_cert = std::move(client_cert),
                                                     .client_key = std::move(client_key)};
  const auto broker = Read(invocation, "--mqtt");
  if (!result.Empty() && (!broker || !broker->starts_with("ssl://")))
    throw std::invalid_argument("MQTT TLS credentials require --mqtt ssl://host:port.");
  if (result.client_cert.empty() != result.client_key.empty())
    throw std::invalid_argument("MQTT client certificate and key must be supplied together.");
  for (const auto* path : {&result.truststore, &result.client_cert, &result.client_key})
    if (!path->empty() && !std::filesystem::is_regular_file(*path))
      throw std::invalid_argument("MQTT TLS credential file does not exist: " + *path);
  return result;
}
}  // namespace

void AddDiscoveryOptions(Command& command) {
  command.AddOption("--registry", {}, {}, "Query local runtime registrations (default)");
  command.AddOption("--registry-path", {}, "path",
                    "Local registry root (default: " +
                        extension::DefaultRuntimeRegistryPath().string() + ")",
                    OptionValueMode::kRequired);
  command.AddOption("--include-hidden-services", {}, {}, "Consider hidden services as well");
  command.AddOption("--dds", "-d", {}, "Discover DDS request/reply endpoints");
  command.AddOption("--ros2", "-r", {}, "Query native ROS 2 services");
  command.AddOption("--zenoh", "-z", {}, "Query IBMW Zenoh service directories");
  command.AddOption("--http", {}, "host:port", "Query an IBMW HTTP server directory", OptionValueMode::kRequired);
  command.AddOption("--grpc", {}, "host:port", "Query an IBMW gRPC server directory (HTTP/2)", OptionValueMode::kRequired);
  command.AddOption("--mqtt", {}, "broker-uri", "Query IBMW services through an MQTT broker", OptionValueMode::kRequired);
  command.AddOption("--mqtt-truststore", {}, "path", "MQTT TLS CA certificate (otherwise system trust store)", OptionValueMode::kRequired);
  command.AddOption("--mqtt-client-cert", {}, "path", "MQTT TLS client certificate", OptionValueMode::kRequired);
  command.AddOption("--mqtt-client-key", {}, "path", "MQTT TLS client private key", OptionValueMode::kRequired);
  command.AddOption("--domain", {}, "id", "Domain for selected DDS/ROS 2 sources", OptionValueMode::kRequired);
  command.AddOption("--dds-domain", {}, "id", "DDS domain (overrides --domain; default: 0)", OptionValueMode::kRequired);
  command.AddOption("--ros-domain", {}, "id", "ROS 2 domain (otherwise --domain, ROS_DOMAIN_ID or 0)", OptionValueMode::kRequired);
  command.AddOption("--zenoh-domain", {}, "name", "Zenoh limit_domain", OptionValueMode::kRequired);
  command.AddOption("--zenoh-config", {}, "path", "Zenoh native connection configuration", OptionValueMode::kRequired);
  command.AddOption("--timeout", {}, "seconds", "Discovery deadline, including connections (default: 2)", OptionValueMode::kRequired);
}

std::vector<ibmw::discovery::ServiceDiscoveryOptions> ParseDiscoveryOptions(const Invocation& invocation) {
  using Backend = ibmw::discovery::ServiceDiscoveryBackend;
  using Options = ibmw::discovery::ServiceDiscoveryOptions;
  const bool dds = invocation.HasOption("--dds"), ros2 = invocation.HasOption("--ros2");
  const bool zenoh = invocation.HasOption("--zenoh");
  const auto http = Read(invocation, "--http"), grpc = Read(invocation, "--grpc"), mqtt = Read(invocation, "--mqtt");
  const auto mqtt_tls = ParseDiscoveryTlsOptions(invocation);
  const bool registry = invocation.HasOption("--registry") || !(dds || ros2 || zenoh || http || grpc || mqtt);
  const auto common = Read(invocation, "--domain"), dd = Read(invocation, "--dds-domain"), rd = Read(invocation, "--ros-domain");
  const auto zd = Read(invocation, "--zenoh-domain"), zc = Read(invocation, "--zenoh-config");
  if ((common && !(dds || ros2)) || (dd && !dds) || (rd && !ros2) || ((zd || zc) && !zenoh) ||
      (invocation.HasOption("--registry-path") && !registry))
    throw std::invalid_argument("Discovery configuration requires its corresponding source to be selected.");
  std::vector<Options> result;
  if (registry) result.push_back({.registry_path = ParseRegistryPath(invocation)});
  if (dds) {
    const uint32_t dds_domain = Domain(dd.value_or(common.value_or("0")));
    result.push_back({.backend = Backend::kDds, .domain_id = dds_domain});
  }
  if (ros2) {
    const char* env = std::getenv("ROS_DOMAIN_ID");
    const uint32_t ros_domain = Domain(rd.value_or(common.value_or(env && *env ? env : "0")));
    result.push_back({.backend = Backend::kRos2, .domain_id = ros_domain});
  }
  if (zenoh) result.push_back({.backend = Backend::kZenoh, .zenoh_domain = zd.value_or(""), .zenoh_config = zc.value_or("")});
  if (http) result.push_back({.backend = Backend::kHttp, .endpoint = *http});
  if (grpc) result.push_back({.backend = Backend::kGrpc, .endpoint = *grpc});
  if (mqtt) result.push_back({.backend = Backend::kMqtt, .endpoint = *mqtt, .mqtt_tls = mqtt_tls});
  return result;
}

std::chrono::milliseconds ParseDiscoveryTimeout(const Invocation& invocation) {
  const auto value = Read(invocation, "--timeout");
  if (!value) return std::chrono::seconds(2);
  double seconds = 0;
  const auto [last, error] = std::from_chars(value->data(), value->data() + value->size(), seconds);
  if (error != std::errc{} || last != value->data() + value->size() || !std::isfinite(seconds) ||
      seconds < 0.01 || seconds > 60)
    throw std::invalid_argument("--timeout must be between 0.01 and 60 seconds.");
  return std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
}

std::filesystem::path ParseRegistryPath(const Invocation& invocation) {
  const auto found = invocation.options.find("--registry-path");
  if (found == invocation.options.end()) return extension::DefaultRuntimeRegistryPath();
  if (found->second.size() != 1 || found->second.front().empty()) {
    throw std::invalid_argument("--registry-path requires exactly one nonempty value.");
  }
  return found->second.front();
}

}  // namespace ibmw::cli::service
