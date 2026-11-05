// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_options.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unistd.h>
#include "base/url_parser.h"

namespace ibmw::cli::service {
namespace {
std::string Value(const Invocation& in, const std::string& key, std::string fallback = {}) {
  const auto it = in.options.find(key);
  if (it == in.options.end()) return fallback;
  if (it->second.size() != 1 || it->second.front().empty())
    throw std::invalid_argument(key + " requires one nonempty value.");
  return it->second.front();
}
uint32_t Integer(const std::string& text, const char* name, uint32_t low, uint32_t high) {
  uint32_t n = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
  if (ec != std::errc{} || end != text.data() + text.size() || n < low || n > high)
    throw std::invalid_argument(std::string("Invalid ") + name + ".");
  return n;
}
std::chrono::milliseconds Duration(const Invocation& in, const char* name, const char* fallback) {
  const auto text = Value(in, name, fallback);
  double n = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
  if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(n) || n < 0.01 || n > 3600)
    throw std::invalid_argument(std::string(name) + " must be between 0.01 and 3600 seconds.");
  return std::chrono::milliseconds(static_cast<int64_t>(n * 1000));
}
std::string Endpoint(std::string value, std::string_view scheme) {
  if (value.find("://") == std::string::npos) value = std::string(scheme) + "://" + value;
  const auto url = ibmw::common::util::ParseUrl(value);
  if (!url || url->protocol != scheme || url->host.empty() ||
      (!url->path.empty() && url->path != "/") || !url->query.empty() || !url->fragment.empty() ||
      url->host.find_first_of("@:[] \t\r\n") != std::string_view::npos)
    throw std::invalid_argument("Expected " + std::string(scheme) + "://host:port.");
  Integer(std::string(url->service), "endpoint port", 1, 65535);
  return std::string(scheme) + "://" + std::string(url->host) + ":" + std::string(url->service);
}
}  // namespace

std::string CallLibrary(std::string_view filename) {
  static const char anchor = 0;
  Dl_info info{};
  if (!::dladdr(&anchor, &info) || !info.dli_fname) throw std::runtime_error("Cannot locate service call libraries.");
  const auto directory = std::filesystem::absolute(info.dli_fname).parent_path();
  for (const auto& path : {directory / filename, directory.parent_path() / "lib" / filename})
    if (std::filesystem::is_regular_file(path)) return path.string();
  throw std::runtime_error("Service call library is not installed: " + std::string(filename));
}

CallOptions ParseCallOptions(const Invocation& in) {
  CallOptions out;
  for (const auto& [key, values] : in.options)
    if (key != "--type-support" && values.size() != 1)
      throw std::invalid_argument(key + " must be specified once.");
  unsigned selected = 0;
  for (const auto* backend : {"dds", "ros2", "http", "grpc", "mqtt", "zenoh"}) {
    if (in.HasOption(std::string("--") + backend)) { out.backend = backend; ++selected; }
  }
  if (selected != 1) throw std::invalid_argument("call requires exactly one backend: --dds, --ros2, --http, --grpc, --mqtt or --zenoh. Registry/local alone cannot call another process.");
  const auto& b = out.backend;
  const auto require = [&](const char* option, bool valid) {
    if (in.HasOption(option) && !valid) throw std::invalid_argument(std::string(option) + " does not apply to --" + b + ".");
  };
  require("--domain", b == "dds" || b == "ros2");
  require("--dds-domain", b == "dds"); require("--ros-domain", b == "ros2");
  require("--ros2-compatible", b == "dds");
  require("--serialization", b != "dds" && b != "ros2");
  require("--zenoh-domain", b == "zenoh"); require("--zenoh-config", b == "zenoh");
  for (const char* option : {"--mqtt-client-id", "--mqtt-server-id", "--mqtt-qos", "--mqtt-truststore", "--mqtt-client-cert", "--mqtt-client-key"}) require(option, b == "mqtt");
  out.service = in.positional_args.at(0); out.type = in.positional_args.at(1); out.request = in.positional_args.at(2);
  if (out.service.empty() || out.type.empty()) throw std::invalid_argument("Service and type must not be empty.");
  if (out.service.find_first_of(" \t\r\n") != std::string::npos) throw std::invalid_argument("Service name must not contain whitespace.");
  out.timeout = Duration(in, "--timeout", "5");
  out.discovery_timeout = Duration(in, "--discovery-timeout", "2");
  out.serialization = Value(in, "--serialization");
  if (auto it = in.options.find("--type-support"); it != in.options.end()) {
    for (const auto& path : it->second) {
      if (path.empty()) throw std::invalid_argument("--type-support requires a nonempty library path.");
      out.packages.push_back(path);
    }
  }
  auto runtime = out.root["ibmw"];
  YAML::Node extension, driver, client;
  extension["name"] = (b == "http" ? "net" : b) + "_extension";
  // Resolve paths after payload/type validation, before Engine initialization.
  extension["path"] = Value(in, "--transport-library");
  auto settings = extension["options"];
  settings = YAML::Node(YAML::NodeType::Map);
  driver["type"] = b;
  auto options = driver["options"];
  options = YAML::Node(YAML::NodeType::Map);
  const auto identity = "ibmw_service_call_" + std::to_string(::getpid());
  client["func_name"] = ".*";
  if (b == "dds" || b == "ros2") {
    const char* env = std::getenv("ROS_DOMAIN_ID");
    auto domain = Value(in, "--domain", b == "ros2" && env && *env ? env : "0");
    domain = Value(in, b == "dds" ? "--dds-domain" : "--ros-domain", domain);
    settings["domain_id"] = Integer(domain, "domain", 0, std::numeric_limits<uint32_t>::max());
    settings[b == "dds" ? "participant_name" : "node_name"] = identity;
    if (b == "dds") options["ros2_compatible"] = in.HasOption("--ros2-compatible");
    else {
      // A distinct internal method plus remapping preserves literal ROS names,
      // including underscores, instead of the driver's IBMW name encoding.
      client["remapping_rule"] = out.service.starts_with("ros2:") ? out.service.substr(5) : out.service;
    }
  } else if (b == "http" || b == "grpc") {
    client["server_url"] = Endpoint(Value(in, "--" + b), "http");
    settings["thread_num"] = 2;
  } else if (b == "zenoh") {
    settings["limit_domain"] = Value(in, "--zenoh-domain");
    if (in.HasOption("--zenoh-config")) settings["native_cfg_path"] = std::filesystem::absolute(Value(in, "--zenoh-config")).string();
    client["shm_enabled"] = false;
  } else {
    auto broker = Value(in, "--mqtt");
    const bool tls = broker.starts_with("ssl://") || broker.starts_with("mqtts://");
    if (broker.starts_with("mqtt://")) broker.replace(0, 7, "tcp://");
    if (broker.starts_with("mqtts://")) broker.replace(0, 8, "ssl://");
    settings["broker_addr"] = Endpoint(broker, tls ? "ssl" : "tcp");
    settings["client_id"] = Value(in, "--mqtt-client-id", identity);
    settings["callback_executor_name"] = "call_worker";
    client["qos"] = Integer(Value(in, "--mqtt-qos", "2"), "MQTT QoS", 0, 2);
    client["server_mqtt_id"] = Value(in, "--mqtt-server-id");
    if (tls && !in.HasOption("--mqtt-truststore")) throw std::invalid_argument("TLS MQTT requires --mqtt-truststore.");
    for (const auto& [flag, key] : {std::pair{"--mqtt-truststore", "truststore"}, {"--mqtt-client-cert", "client_cert"}, {"--mqtt-client-key", "client_key"}}) {
      if (!in.HasOption(flag)) continue;
      if (!tls) throw std::invalid_argument(std::string(flag) + " requires a TLS broker.");
      settings[key] = Value(in, flag);
    }
    if (in.HasOption("--mqtt-client-cert") != in.HasOption("--mqtt-client-key")) throw std::invalid_argument("MQTT client certificate and key must be supplied together.");
  }
  options["clients"].push_back(client);
  if (b != "http" && b != "grpc") {
    options["timeout_executor"] = "call_timer";
    runtime["scheduling"]["executors"] = YAML::Load("[{name: call_worker, type: thread_pool, options: {thread_num: 2}}, {name: call_timer, type: timer, options: {bind_executor: call_worker}}]");
  }
  runtime["extensions"]["items"].push_back(extension);
  runtime["service"]["drivers"].push_back(driver);
  YAML::Node route; route["func_name"] = ".*"; route["enable_drivers"].push_back(b);
  runtime["service"]["clients"].push_back(route);
  runtime["runtime_graph"]["enabled"] = false;
  runtime["logging"]["core_level"] = "WARN";
  runtime["logging"]["backends"] = YAML::Node(YAML::NodeType::Sequence);
  return out;
}

void AddCallTransportOptions(Command& command, bool native_ros2) {
  using Mode = OptionValueMode;
  command.AddOption("--dds", "-d", {}, "Call through DDS");
  if (native_ros2) command.AddOption("--ros2", "-r", {}, "Call a native ROS 2 service");
  command.AddOption("--zenoh", "-z", {}, "Call through Zenoh");
  for (const auto* b : {"http", "grpc", "mqtt"}) command.AddOption(std::string("--") + b, {}, "endpoint", std::string("Call through ") + b, Mode::kRequired);
  for (const auto& [name, description] : {
      std::pair{"--domain", "DDS/ROS 2 domain"}, {"--dds-domain", "DDS domain (overrides --domain)"},
      {"--ros-domain", "ROS 2 domain (otherwise --domain, ROS_DOMAIN_ID or 0)"},
      {"--zenoh-domain", "Zenoh limit_domain"}, {"--zenoh-config", "Zenoh native configuration file"},
      {"--transport-library", "Override the selected extension library"},
      {"--serialization", "Wire serialization; defaults to a format supported by both types"},
      {"--timeout", "Response deadline in seconds (default: 5); requests are not retried"},
      {"--discovery-timeout", "Client readiness deadline in seconds (default: 2)"},
      {"--mqtt-client-id", "MQTT client identifier (default: unique process ID)"},
      {"--mqtt-server-id", "Optional MQTT server identifier"}, {"--mqtt-qos", "MQTT QoS 0, 1 or 2 (default: 2)"},
      {"--mqtt-truststore", "MQTT TLS CA certificate"}, {"--mqtt-client-cert", "MQTT TLS client certificate"},
      {"--mqtt-client-key", "MQTT TLS client private key"}})
    if (native_ros2 || std::string_view(name) != "--ros-domain")
      command.AddOption(name, {}, "value", description, Mode::kRequired);
  if (native_ros2) command.AddOption("--ros2-compatible", {}, {}, "DDS: use ROS 2 request/reply topic naming");
}

void AddCallOptions(Command& command) {
  command.AddArgument("<service>", "Full service/method name");
  command.AddArgument("<type>", "'request_type -> response_type', or pkg/srv/Type for --ros2");
  command.AddArgument("<request>", "One quoted JSON/YAML request object");
  AddCallTransportOptions(command);
  command.AddOption("--type-support", {}, "library", "Request/response TypeSupport library; repeatable",
                    OptionValueMode::kRequired);
  command.AddOption("--include-hidden-services", {}, {}, "Explicit calls already allow hidden service names");
}
}  // namespace ibmw::cli::service
