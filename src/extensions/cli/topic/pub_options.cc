// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "pub_options.h"

#include <arpa/inet.h>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

#include "base/url_parser.h"
#include "engine/topic_io/transport_library.h"
#include "command_options.h"

namespace ibmw::cli::topic {
namespace {
constexpr const char* kDrivers[] = {"dds", "ros2", "zenoh", "mqtt", "tcp", "udp", "http", "iceoryx", "shm", "local"};

uint64_t PositiveInteger(const Invocation& invocation, const std::string& name, const char* fallback) {
  const auto value = OptionValue(invocation, name, fallback);
  uint64_t number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size() || number == 0)
    throw std::invalid_argument(name + " requires a positive integer.");
  return number;
}

double Number(const Invocation& invocation, const std::string& name, const char* fallback,
              double minimum, double maximum) {
  const auto value = OptionValue(invocation, name, fallback);
  double number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number) ||
      number < minimum || number > maximum)
    throw std::invalid_argument("Invalid value for " + name + ".");
  return number;
}

uint64_t Integer(std::string_view text, std::string_view option, uint64_t min, uint64_t max) {
  uint64_t number = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
  if (error != std::errc{} || end != text.data() + text.size() || number < min || number > max)
    throw std::invalid_argument(std::string(option) + " requires a decimal integer in [" +
                                std::to_string(min) + ", " + std::to_string(max) + "].");
  return number;
}

std::string RequiredValue(const Invocation& invocation, const char* option, const std::string& fallback = {}) {
  auto value = OptionValue(invocation, option, fallback);
  if (value.empty()) throw std::invalid_argument(std::string(option) + " requires a nonempty value.");
  return value;
}

void ValidateTarget(const std::string& value, const std::string& driver) {
  const auto url = ibmw::common::util::ParseUrl(value);
  if (!url || url->host.empty() || !url->query.empty() || !url->fragment.empty() ||
      (driver == "http" ? url->protocol != "http" || (!url->path.empty() && url->path != "/")
                        : !url->protocol.empty() || !url->path.empty()))
    throw std::invalid_argument("--target requires " + std::string(driver == "http" ? "http://host:port" : "IPv4:port") + ".");
  Integer(url->service, "--target port", 1, 65535);
  if (driver != "http") {
    in_addr address{};
    if (::inet_pton(AF_INET, std::string(url->host).c_str(), &address) != 1)
      throw std::invalid_argument("--target requires a numeric IPv4 address for TCP/UDP.");
  } else if (url->host.find_first_of("@:[] \t\r\n") != std::string_view::npos) {
    throw std::invalid_argument("--target requires an HTTP host without credentials or IPv6 brackets.");
  }
}
}  // namespace

PublicationConfig ParsePublicationOptions(const Invocation& invocation) {
  for (const auto& [name, values] : invocation.options) {
    if (name != "--type-support" && name != "--target" && values.size() != 1)
      throw std::invalid_argument(name + " must be specified exactly once.");
  }
  PublicationConfig result;
  unsigned selected = 0;
  for (const auto* driver : kDrivers) {
    if (invocation.HasOption(std::string("--") + driver)) {
      result.driver = driver;
      ++selected;
    }
  }
  if (selected != 1)
    throw std::invalid_argument("pub requires exactly one publishing backend: --dds, --ros2, --zenoh, --mqtt, --tcp, --udp, --http, --iceoryx, --shm or --local.");
  const auto& driver = result.driver;
  const bool dds_or_ros = driver == "dds" || driver == "ros2";
  const bool network = driver == "tcp" || driver == "udp" || driver == "http";
  const auto require = [&](const char* option, bool supported) {
    if (invocation.HasOption(option) && !supported)
      throw std::invalid_argument(std::string(option) + " does not apply to --" + driver + ".");
  };
  require("--domain", dds_or_ros);
  require("--dds-domain", driver == "dds");
  require("--ros-domain", driver == "ros2");
  require("--ros2-compatible", driver == "dds");
  require("--zenoh-domain", driver == "zenoh");
  require("--zenoh-config", driver == "zenoh");
  require("--target", network);
  require("--net-threads", network);
  require("--transport-library", driver != "local");
  for (const auto* option : {"--broker", "--client-id", "--mqtt-qos", "--mqtt-truststore", "--mqtt-client-cert", "--mqtt-client-key", "--mqtt-key-password"})
    require(option, driver == "mqtt");
  require("--iceoryx-runtime-id", driver == "iceoryx");
  require("--shm-init-size", driver == "iceoryx" || driver == "shm");
  for (const auto* option : {"--shm-runtime-id", "--shm-domain", "--shm-root-dir", "--shm-group",
                             "--shm-max-bytes-per-publisher", "--shm-prefault", "--shm-require-read-only"})
    require(option, driver == "shm");
  for (const auto* option : {"--qos-reliability", "--qos-durability", "--qos-history", "--qos-depth"}) {
    if (invocation.HasOption(option) && !dds_or_ros)
      throw std::invalid_argument(std::string(option) + " only applies to DDS/ROS 2.");
  }

  auto runtime = result.root["ibmw"];
  YAML::Node extension, transport, publisher;
  extension["name"] = (network ? "net" : driver) + "_extension";
  auto settings = extension["options"];
  transport["type"] = driver;
  auto options = transport["options"];
  publisher["topic"] = ".*";  // This Engine owns one publisher.
  const auto identity = "ibmw_topic_pub_" + std::to_string(::getpid());
  if (dds_or_ros) {
    std::string domain = OptionValue(invocation, "--domain", "0");
    if (driver == "ros2" && !invocation.HasOption("--domain")) {
      if (const char* value = std::getenv("ROS_DOMAIN_ID"); value && *value) domain = value;
    }
    const char* option = driver == "dds" ? "--dds-domain" : "--ros-domain";
    domain = OptionValue(invocation, option, domain);
    settings["domain_id"] = Integer(domain, option, 0, std::numeric_limits<uint32_t>::max());
    settings[driver == "dds" ? "participant_name" : "node_name"] = identity;
    for (const auto& policy : {std::string("reliability"), std::string("durability"), std::string("history")}) {
      const auto key = "--qos-" + policy;
      const auto value = OptionValue(invocation, key,
          policy == "reliability" ? "reliable" : policy == "durability" ? "volatile" : "keep_last");
      if (!(policy == "reliability" ? value == "reliable" || value == "best_effort" :
            policy == "durability" ? value == "volatile" || value == "transient_local" :
                                     value == "keep_last" || value == "keep_all"))
        throw std::invalid_argument("Invalid value for " + key + ".");
      publisher["qos"][policy] = value;
    }
    publisher["qos"]["depth"] = Integer(OptionValue(invocation, "--qos-depth", "10"), "--qos-depth", 1, std::numeric_limits<int32_t>::max());
    if (invocation.HasOption("--ros2-compatible")) options["ros2_compatible"] = true;
  } else if (driver == "zenoh") {
    settings["limit_domain"] = OptionValue(invocation, "--zenoh-domain");
    if (invocation.HasOption("--zenoh-config"))
      settings["native_cfg_path"] = std::filesystem::absolute(RequiredValue(invocation, "--zenoh-config")).string();
    publisher["shm_enabled"] = false;
  } else if (network) {
    const auto targets = invocation.options.find("--target");
    if (targets == invocation.options.end() || targets->second.empty())
      throw std::invalid_argument("--" + driver + " requires --target.");
    for (const auto& target : targets->second) {
      ValidateTarget(target, driver);
      publisher["server_url_list"].push_back(target);
    }
    settings["thread_num"] = Integer(OptionValue(invocation, "--net-threads", "2"), "--net-threads", 1, 256);
  } else if (driver == "mqtt") {
    const auto broker = RequiredValue(invocation, "--broker");
    const auto url = ibmw::common::util::ParseUrl(broker);
    if (!url || url->host.empty() || (url->protocol != "tcp" && url->protocol != "mqtt" && url->protocol != "ssl" && url->protocol != "mqtts") ||
        !url->path.empty() || !url->query.empty() || !url->fragment.empty() || url->host.find_first_of("@:[] \t\r\n") != std::string_view::npos)
      throw std::invalid_argument("--broker requires tcp://host:port or ssl://host:port.");
    Integer(url->service, "--broker port", 1, 65535);
    settings["broker_addr"] = (url->protocol == "mqtt" ? "tcp" : url->protocol == "mqtts" ? "ssl" : std::string(url->protocol)) +
                              "://" + std::string(url->host) + ":" + std::string(url->service);
    settings["client_id"] = RequiredValue(invocation, "--client-id", identity);
    publisher["qos"] = Integer(OptionValue(invocation, "--mqtt-qos", "2"), "--mqtt-qos", 0, 2);
    const bool tls = url->protocol == "ssl" || url->protocol == "mqtts";
    if (tls && !invocation.HasOption("--mqtt-truststore"))
      throw std::invalid_argument("A TLS broker requires --mqtt-truststore.");
    for (const auto& [option, key] : {std::pair{"--mqtt-truststore", "truststore"}, {"--mqtt-client-cert", "client_cert"}, {"--mqtt-client-key", "client_key"}, {"--mqtt-key-password", "client_key_password"}}) {
      if (!invocation.HasOption(option)) continue;
      if (!tls) throw std::invalid_argument(std::string(option) + " requires a TLS broker URL.");
      settings[key] = RequiredValue(invocation, option);
    }
    if (invocation.HasOption("--mqtt-client-cert") != invocation.HasOption("--mqtt-client-key"))
      throw std::invalid_argument("--mqtt-client-cert and --mqtt-client-key must be supplied together.");
    if (invocation.HasOption("--mqtt-key-password") && !invocation.HasOption("--mqtt-client-key"))
      throw std::invalid_argument("--mqtt-key-password requires --mqtt-client-key.");
  } else if (driver == "iceoryx") {
    settings["runtime_id"] = RequiredValue(invocation, "--iceoryx-runtime-id", identity);
    settings["shm_init_size"] = Integer(OptionValue(invocation, "--shm-init-size", "1024"), "--shm-init-size", 1, std::numeric_limits<uint32_t>::max());
  } else if (driver == "shm") {
    // Keys of the shm_extension YAML node; omitted ones keep its defaults.
    settings["runtime_id"] = RequiredValue(invocation, "--shm-runtime-id", identity);
    settings["shm_init_size"] = Integer(OptionValue(invocation, "--shm-init-size", "1024"), "--shm-init-size", 1, std::numeric_limits<uint32_t>::max());
    if (invocation.HasOption("--shm-domain")) settings["domain"] = RequiredValue(invocation, "--shm-domain");
    if (invocation.HasOption("--shm-root-dir")) settings["root_dir"] = RequiredValue(invocation, "--shm-root-dir");
    if (invocation.HasOption("--shm-group")) {
      // 4294967295 is (gid_t)-1, "no change" for fchown, so it is not a group.
      settings["group"] = std::to_string(Integer(OptionValue(invocation, "--shm-group", ""), "--shm-group", 0,
                                                 std::numeric_limits<uint32_t>::max() - 1));
    }
    if (invocation.HasOption("--shm-max-bytes-per-publisher")) {
      settings["max_bytes_per_publisher"] = PositiveInteger(invocation, "--shm-max-bytes-per-publisher", "");
    }
    if (invocation.HasOption("--shm-prefault")) settings["prefault"] = true;
    if (invocation.HasOption("--shm-require-read-only")) settings["require_read_only"] = true;
  }
  if (driver != "local") {
    extension["path"] = invocation.HasOption("--transport-library")
        ? RequiredValue(invocation, "--transport-library")
        : ibmw::runtime::core::topic_io::TransportLibrary(network ? "net" : driver).string();
    runtime["extensions"]["items"].push_back(extension);
  }
  // Local, Iceoryx and shm do not have per-topic driver publisher options.
  if (driver != "local" && driver != "iceoryx" && driver != "shm") options["publishers"].push_back(publisher);
  runtime["messaging"]["drivers"].push_back(transport);
  YAML::Node route;
  route["topic"] = ".*";
  route["enable_drivers"].push_back(driver);
  runtime["messaging"]["publishers"].push_back(route);
  runtime["runtime_graph"]["enabled"] = true;
  runtime["logging"]["core_level"] = "WARN";
  YAML::Node console;
  console["type"] = "console";
  runtime["logging"]["backends"].push_back(console);
  result.topic = invocation.positional_args.at(0);
  result.type_name = invocation.positional_args.at(1);
  if (result.topic.empty() || result.type_name.empty()) throw std::invalid_argument("Topic and type must not be empty.");
  result.rate = Number(invocation, "--rate", "1", 0.000001, 1000000);
  result.wait = Number(invocation, "--wait", "1", 0, 86400);
  result.keep_alive = Number(invocation, "--keep-alive", "0.2", 0, 86400);
  if (invocation.HasOption("--once") && invocation.HasOption("--times"))
    throw std::invalid_argument("--once and --times are mutually exclusive.");
  result.count = invocation.HasOption("--once") ? 1 : invocation.HasOption("--times")
      ? PositiveInteger(invocation, "--times", "1") : std::numeric_limits<uint64_t>::max();
  return result;
}

void AddPublicationOptions(Command& pub) {
  using Mode = OptionValueMode;
  pub.AddArgument("<topic>", "Topic to publish (may be new)");
  pub.AddArgument("<type>", "Canonical message type, including dds:, ros2: or json:");
  pub.AddArgument("<values>", "Quoted JSON/YAML message contents");
  for (const auto* driver : kDrivers) {
    const std::string name(driver);
    pub.AddOption("--" + name, name == "dds" ? "-d" : name == "ros2" ? "-r" : name == "zenoh" ? "-z" : "", {},
                  name == "local" ? "Publish inside this CLI process only" : "Publish through " + name);
  }
  pub.AddOption("--ros2-compatible", {}, {}, "Use ROS 2 wire topic naming for ros2: messages over DDS");
  pub.AddOption("--domain", {}, "id", "DDS/ROS 2 domain", Mode::kRequired);
  pub.AddOption("--dds-domain", {}, "id", "DDS domain (overrides --domain; default: 0)", Mode::kRequired);
  pub.AddOption("--ros-domain", {}, "id", "ROS 2 domain (overrides --domain; otherwise ROS_DOMAIN_ID or 0)", Mode::kRequired);
  pub.AddOption("--zenoh-domain", {}, "name", "Zenoh limit_domain", Mode::kRequired);
  pub.AddOption("--zenoh-config", {}, "path", "Optional Zenoh native connection configuration", Mode::kRequired);
  pub.AddOption("--target", {}, "address", "TCP/UDP IPv4:port or HTTP http://host:port; repeatable", Mode::kRequired);
  pub.AddOption("--net-threads", {}, "count", "TCP/UDP/HTTP worker threads (default: 2)", Mode::kRequired);
  pub.AddOption("--broker", {}, "url", "MQTT broker, e.g. tcp://127.0.0.1:1883", Mode::kRequired);
  pub.AddOption("--client-id", {}, "name", "MQTT client ID (default: unique process ID)", Mode::kRequired);
  pub.AddOption("--mqtt-qos", {}, "level", "MQTT QoS: 0, 1 or 2 (default: 2)", Mode::kRequired);
  pub.AddOption("--mqtt-truststore", {}, "path", "MQTT TLS CA certificate", Mode::kRequired);
  pub.AddOption("--mqtt-client-cert", {}, "path", "MQTT TLS client certificate", Mode::kRequired);
  pub.AddOption("--mqtt-client-key", {}, "path", "MQTT TLS client private key", Mode::kRequired);
  pub.AddOption("--mqtt-key-password", {}, "password", "MQTT TLS client key password", Mode::kRequired);
  pub.AddOption("--iceoryx-runtime-id", {}, "name", "Iceoryx runtime ID (default: unique process ID)", Mode::kRequired);
  pub.AddOption("--shm-init-size", {}, "bytes", "Iceoryx/shm initial shared-memory chunk size (default: 1024)", Mode::kRequired);
  pub.AddOption("--shm-runtime-id", {}, "name", "shm runtime ID (default: unique process ID)", Mode::kRequired);
  pub.AddOption("--shm-domain", {}, "name", "shm domain (default: IBMW_SHM_DOMAIN)", Mode::kRequired);
  pub.AddOption("--shm-root-dir", {}, "path", "shm discovery root directory (default: IBMW_SHM_ROOT or the built-in root)", Mode::kRequired);
  pub.AddOption("--shm-group", {}, "gid", "shm numeric group ID sharing the memory (default: IBMW_SHM_GROUP or user-private)", Mode::kRequired);
  pub.AddOption("--shm-max-bytes-per-publisher", {}, "bytes", "shm shared-memory budget per publisher (default: 512 MiB)", Mode::kRequired);
  pub.AddOption("--shm-prefault", {}, {}, "shm: prefault new shared-memory segments");
  pub.AddOption("--shm-require-read-only", {}, {}, "shm: fail instead of creating segments subscribers could map writable");
  pub.AddOption("--transport-library", {}, "path", "Override the selected transport extension library", Mode::kRequired);
  pub.AddOption("--qos-reliability", {}, "policy", "DDS/ROS 2: reliable (default) or best_effort", Mode::kRequired);
  pub.AddOption("--qos-durability", {}, "policy", "DDS/ROS 2: volatile (default) or transient_local", Mode::kRequired);
  pub.AddOption("--qos-history", {}, "policy", "DDS/ROS 2: keep_last (default) or keep_all", Mode::kRequired);
  pub.AddOption("--qos-depth", {}, "count", "DDS/ROS 2 history depth (default: 10)", Mode::kRequired);
  pub.AddOption(
      "--type-support", {}, "path", "Message TypeSupport library; repeatable",
      Mode::kRequired);
  pub.AddOption("--once", {}, {}, "Publish one message and exit");
  pub.AddOption(
      "--times", "-t", "count", "Publish this many messages and exit",
      Mode::kRequired);
  pub.AddOption(
      "--rate", {}, "hz", "Frequency (default: 1 Hz, until Ctrl+C)",
      Mode::kRequired);
  pub.AddOption(
      "--wait", {}, "seconds",
      "Connection/discovery delay before sending (default: 1); no matching guarantee",
      Mode::kRequired);
  pub.AddOption(
      "--keep-alive", {}, "seconds",
      "Remain alive after the last send (default: 0.2); not an acknowledgment",
      Mode::kRequired);
}
}  // namespace ibmw::cli::topic
