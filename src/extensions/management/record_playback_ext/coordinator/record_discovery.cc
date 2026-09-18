// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/record_discovery.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <map>
#include <unistd.h>

#include "base/exception.h"
#include "engine/discovery/registry_discovery_provider.h"

namespace ibmw::extensions::record_playback_extension {
using ibmw::discovery::DiscoveryBackend;
using ibmw::discovery::EndpointRole;

bool RecordDiscoveryOptions::ParseArgument(int& index, int argc, const char* const* argv) {
  const std::string argument = argv[index];
  for (const auto backend : {DiscoveryBackend::kRegistry, DiscoveryBackend::kDds,
                             DiscoveryBackend::kRos2, DiscoveryBackend::kZenoh}) {
    if (argument != "--" + std::string(ibmw::discovery::BackendName(backend))) continue;
    MW_ASSERT(!selected_backend || *selected_backend == backend,
              "bag record accepts one discovery source: --registry, --dds, --ros2 or --zenoh.");
    selected_backend = source.backend = backend;
    return true;
  }
  const auto separator = argument.find('=');
  const auto name = argument.substr(0, separator);
  if (name != "--domain" && name != "--zenoh-domain" &&
      name != "--zenoh-config" && name != "--config-file" && name != "--registry-path") return false;
  std::string value;
  if (separator == std::string::npos) {
    MW_ASSERT(index + 1 < argc, "{} requires a value.", name);
    value = argv[++index];
  } else {
    value = argument.substr(separator + 1);
  }
  MW_ASSERT(!value.empty(), "{} requires a nonempty value.", name);
  if (name == "--domain") {
    MW_ASSERT(!domain, "--domain must be specified once.");
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    MW_ASSERT(error == std::errc{} && end == value.data() + value.size(),
              "--domain requires an integer in [0, 4294967295].");
    domain = parsed;
  } else if (name == "--zenoh-domain") {
    MW_ASSERT(!zenoh_domain, "--zenoh-domain must be specified once.");
    zenoh_domain = value;
  } else if (name == "--zenoh-config") {
    MW_ASSERT(source.zenoh_config.empty(), "--zenoh-config must be specified once.");
    source.zenoh_config = std::filesystem::absolute(value);
  } else if (name == "--config-file") {
    MW_ASSERT(config_file.empty(), "--config-file must be specified once.");
    config_file = value;
  } else {
    MW_ASSERT(!registry_selected, "--registry-path must be specified once.");
    registry_selected = true;
    source.registry_path = value;
  }
  return true;
}

void RecordDiscoveryOptions::Validate() const {
  MW_ASSERT(!domain || source.backend == DiscoveryBackend::kDds || source.backend == DiscoveryBackend::kRos2,
            "--domain requires --dds or --ros2.");
  MW_ASSERT(!zenoh_domain || source.backend == DiscoveryBackend::kZenoh,
            "--zenoh-domain requires --zenoh.");
  MW_ASSERT(source.zenoh_config.empty() || source.backend == DiscoveryBackend::kZenoh,
            "--zenoh-config requires --zenoh.");
  MW_ASSERT(Network() || config_file.empty(), "--config-file requires network recording.");
  MW_ASSERT(!Network() || !registry_selected, "--registry-path requires --registry.");
  MW_ASSERT(!Network() || !config_file.empty(),
            "Network recording requires --config-file with transport and TypeSupport settings.");
  MW_ASSERT(config_file.empty() || std::filesystem::is_regular_file(config_file),
            "Recorder config file does not exist: {}", config_file.string());
}

RecordConnection LoadRecordConnection(
    RecordDiscoveryOptions& options,
    runtime::core::runtime_graph::TypeSupportRegistry& types) {
  RecordConnection result;
  if (!options.Network()) return result;
  result.driver = ibmw::discovery::BackendName(options.source.backend);
  const auto path = std::filesystem::absolute(options.config_file);
  const YAML::Node root = YAML::LoadFile(path.string());
  MW_ASSERT(root.IsMap() && root["ibmw"] && root["ibmw"].IsMap(),
            "Recorder config must contain an 'ibmw' map.");
  const auto ibmw = root["ibmw"];
  const auto local_path = [&](const std::string& value) {
    MW_ASSERT(!value.empty(), "Recorder configuration path cannot be empty.");
    std::filesystem::path file(value);
    return (file.is_relative() ? path.parent_path() / file : file).lexically_normal().string();
  };
  const auto extensions = ibmw["extensions"];
  MW_ASSERT(extensions && extensions.IsMap() && extensions["items"].IsSequence(),
            "Recorder config requires ibmw.extensions.items.");
  const std::string extension_name = result.driver + "_extension";
  YAML::Node selected;
  for (const auto& extension : extensions["items"]) {
    if (extension["name"].as<std::string>() != extension_name) continue;
    MW_ASSERT(!selected.IsMap(), "Recorder config contains multiple '{}' items.", extension_name);
    selected = YAML::Clone(extension);
    selected["path"] = local_path(extension["path"].as<std::string>());
  }
  MW_ASSERT(selected.IsMap(), "Recorder config requires a '{}' item.", extension_name);
  auto settings = selected["options"];
  if (!settings || settings.IsNull()) settings = YAML::Node(YAML::NodeType::Map);
  MW_ASSERT(settings.IsMap(), "Transport extension options must be a map.");
  const std::string unique = "ibmw_bag_recorder_" + std::to_string(::getpid());
  if (options.source.backend == DiscoveryBackend::kZenoh) {
    const auto configured = settings["limit_domain"] ? settings["limit_domain"].as<std::string>() : "";
    MW_ASSERT(!options.zenoh_domain || !settings["limit_domain"] || *options.zenoh_domain == configured,
              "--zenoh-domain conflicts with recorder limit_domain.");
    options.source.zenoh_domain = options.zenoh_domain.value_or(configured);
    if (!options.source.zenoh_domain.empty()) settings["limit_domain"] = options.source.zenoh_domain;
    else settings.remove("limit_domain");
    if (settings["native_cfg_path"] && !settings["native_cfg_path"].as<std::string>().empty()) {
      const auto native = local_path(settings["native_cfg_path"].as<std::string>());
      settings["native_cfg_path"] = native;
      if (options.source.zenoh_config.empty()) options.source.zenoh_config = native;
    }
  } else {
    uint32_t configured = 0;
    if (settings["domain_id"]) configured = settings["domain_id"].as<uint32_t>();
    else if (options.source.backend == DiscoveryBackend::kRos2 && !options.domain) {
      if (const auto* value = std::getenv("ROS_DOMAIN_ID")) {
        const std::string text(value);
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), configured);
        MW_ASSERT(error == std::errc{} && end == text.data() + text.size(), "Invalid ROS_DOMAIN_ID.");
      }
    }
    MW_ASSERT(!options.domain || !settings["domain_id"] || *options.domain == configured,
              "--domain conflicts with recorder domain_id.");
    options.source.domain_id = options.domain.value_or(configured);
    settings["domain_id"] = options.source.domain_id;
    settings[options.source.backend == DiscoveryBackend::kDds ? "participant_name" : "node_name"] = unique;
    if (settings["xml_profile_file"] && !settings["xml_profile_file"].as<std::string>().empty()) {
      settings["xml_profile_file"] = local_path(settings["xml_profile_file"].as<std::string>());
    }
  }
  selected["options"] = settings;
  auto runtime = result.runtime_config["ibmw"];
  runtime["runtime_graph"]["enabled"] = false;
  runtime["extensions"]["items"].push_back(selected);
  if (ibmw["scheduling"]) runtime["scheduling"] = YAML::Clone(ibmw["scheduling"]);
  const auto messaging = ibmw["messaging"];
  MW_ASSERT(messaging && messaging.IsMap() && messaging["drivers"].IsSequence(),
            "Recorder config requires ibmw.messaging.drivers.");
  bool found = false;
  for (const auto& driver : messaging["drivers"]) {
    if (driver["type"].as<std::string>() != result.driver) continue;
    MW_ASSERT(!found, "Recorder config contains multiple '{}' drivers.", result.driver);
    found = true;
    runtime["messaging"]["drivers"].push_back(YAML::Clone(driver));
  }
  MW_ASSERT(found, "Recorder config requires a '{}' messaging driver.", result.driver);
  const auto graph = ibmw["runtime_graph"];
  if (graph && graph["type_support_pkgs"]) {
    const auto packages = graph["type_support_pkgs"];
    MW_ASSERT(packages.IsSequence(), "ibmw.runtime_graph.type_support_pkgs must be a sequence.");
    for (const auto& package : packages) {
      types.Load(local_path(package.IsScalar() ? package.as<std::string>() : package["path"].as<std::string>()));
    }
  }
  return result;
}

CaptureGraph ResolveNetworkCaptureGraph(
    const ibmw::discovery::DiscoverySnapshot& snapshot,
    const runtime::core::runtime_graph::TypeSupportRegistry& types) {
  std::vector<CaptureEndpoint> endpoints;
  std::map<std::string, ibmw::transport::QosDescriptor> requests;
  for (const auto& discovered : snapshot.endpoints) {
    CaptureEndpoint endpoint{.driver = std::string(ibmw::discovery::BackendName(discovered.source.backend)),
                             .published = discovered.role == EndpointRole::kPublisher,
                             .hidden = discovered.hidden};
    auto& topic = endpoint.topic;
    switch (discovered.source.backend) {
      case DiscoveryBackend::kDds:
        topic.topic = endpoint.wire_topic_name = discovered.wire_topic_name;
        endpoint.wire_type_name = discovered.wire_type_name;
        topic.msg_type = "dds:" + discovered.wire_type_name;
        topic.serialization_type = "cdr";
        break;
      case DiscoveryBackend::kRos2:
        topic.topic = endpoint.wire_topic_name = discovered.topic_name;
        endpoint.wire_type_name = discovered.type_name;
        topic.msg_type = "ros2:" + discovered.type_name;
        // IBMW names the ROS RMW serialization format "ros2" in its schemas.
        topic.serialization_type = "ros2";
        break;
      case DiscoveryBackend::kZenoh: {
        topic.topic = discovered.topic_name;
        topic.msg_type = endpoint.wire_type_name = discovered.type_name;
        endpoint.wire_topic_name = discovered.wire_topic_name;
        // The wire envelope supplies the actual format for each sample.
        break;
      }
      case DiscoveryBackend::kRegistry:
        MW_ASSERT(false, "Network discovery returned a registry endpoint.");
        break;
    }
    MW_ASSERT(!topic.topic.empty() && !endpoint.wire_topic_name.empty() && !endpoint.wire_type_name.empty(),
              "Network discovery returned an endpoint without subscription addresses.");
    topic.available_drivers = {endpoint.driver};
    if (const auto schema = types.FindSchema(topic.msg_type, topic.serialization_type)) topic.schema = *schema;
    topic.qos = discovered.qos;
    if (endpoint.driver != "zenoh" && endpoint.published) {
      auto [it, inserted] = requests.try_emplace(topic.topic);
      auto& policies = it->second.policies;
      if (inserted) policies = {{"reliability", "reliable"}, {"durability", "transient_local"},
                               {"liveliness", "automatic"}};
      const auto is = [&](const char* key, const char* value) {
        const auto found = discovered.qos.policies.find(key);
        return found != discovered.qos.policies.end() && found->second == value;
      };
      if (!is("reliability", "reliable")) policies["reliability"] = "best_effort";
      if (!is("durability", "transient_local") && !is("durability", "transient") && !is("durability", "persistent"))
        policies["durability"] = "volatile";
    }
    endpoints.push_back(std::move(endpoint));
  }
  CaptureGraph graph;
  for (auto& endpoint : endpoints) {
    const auto offered = endpoint.topic.qos;
    endpoint.topic.qos = requests[endpoint.topic.topic];
    if (endpoint.driver == "dds") {
      const auto ownership = offered.policies.find("ownership");
      endpoint.topic.qos.policies["ownership"] = ownership == offered.policies.end() ? "shared" : ownership->second;
    }
    graph.Add(std::move(endpoint));
  }
  return graph;
}

void ValidateCaptureSchemas(const CaptureGraph& graph,
                            const std::set<std::string>& selected_topics) {
  for (const auto& name : selected_topics) {
    static_cast<void>(graph.Subscriptions(name));
    const bool published = std::any_of(graph.Find(name).begin(), graph.Find(name).end(),
        [](const auto& endpoint) { return endpoint.published; });
    for (const auto& advertisement : graph.Find(name)) {
      if (published && !advertisement.published) continue;
      // Zenoh schemas are checked against the wire format before storing each sample.
      if (!advertisement.registry_advertisement && advertisement.driver == "zenoh" &&
          advertisement.topic.serialization_type.empty()) continue;
      MW_ASSERT(!advertisement.topic.schema.schema_hash.empty(),
                "Cannot record topic '{}' type '{}': no '{}' schema. Add a matching "
                "local package to ibmw.runtime_graph.type_support_pkgs in --config-file.",
                name, advertisement.topic.msg_type, advertisement.topic.serialization_type);
    }
  }
}

RecordDiscovery::RecordDiscovery(
    const RecordDiscoveryOptions& options,
    const runtime::core::runtime_graph::TypeSupportRegistry& types)
    : options_(options.source), types_(types),
      provider_(ibmw::discovery::CreateDiscoveryProvider(options_.backend)) {
  MW_ASSERT(provider_, "Selected record discovery plugin is not installed.");
  provider_->Start(options_);
}

RecordDiscovery::~RecordDiscovery() { provider_->Stop(); }

CaptureGraph RecordDiscovery::Query(std::chrono::milliseconds timeout) {
  const auto snapshot = provider_->Query(std::chrono::steady_clock::now() + timeout);
  for (const auto& status : snapshot.sources) {
    MW_ASSERT(status.status == ibmw::discovery::DiscoveryStatus::kReady,
              "Record discovery failed: {}", status.message);
  }
  MW_ASSERT(snapshot.Succeeded(), "Record discovery returned no source status.");
  if (options_.backend == DiscoveryBackend::kRegistry) {
    const auto* registry = dynamic_cast<const ibmw::discovery::RegistryDiscoveryProvider*>(provider_.get());
    MW_ASSERT(registry, "Registry discovery does not provide Runtime control information.");
    return BuildCaptureGraph(registry->Runtimes());
  }
  return ResolveNetworkCaptureGraph(snapshot, types_);
}

}  // namespace ibmw::extensions::record_playback_extension
