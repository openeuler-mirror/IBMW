// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/topic_subscription.h"

#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unistd.h>

#include "base/log_util.h"
#include "engine/discovery/registry_discovery_provider.h"
#include "engine/engine.h"
#include "engine/topic_io/runtime_tap_stream.h"
#include "engine/topic_io/registry_subscription.h"
#include "engine/topic_io/transport_subscription_source.h"
#include "engine/topic_io/transport_library.h"

namespace ibmw::runtime::core::topic_io {
namespace {
using ibmw::discovery::DiscoveryBackend;
using ibmw::discovery::EndpointRole;

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class TemporaryConfig {
 public:
  explicit TemporaryConfig(const YAML::Node& config) {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "ibmw-topic-XXXXXX").string();
    const int fd = ::mkstemp(pattern.data());
    Require(fd >= 0, "Cannot create topic observer configuration.");
    path_ = pattern;
    ::close(fd);
    try {
      std::ofstream file(path_);
      file << config;
      file.close();
      Require(file.good(), "Cannot write topic observer configuration.");
    } catch (...) {
      std::filesystem::remove(path_);
      throw;
    }
  }
  ~TemporaryConfig() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string Path() const { return path_.string(); }
 private:
  std::filesystem::path path_;
};
}  // namespace

YAML::Node SubscriptionRuntimeConfig(
    ibmw::discovery::DiscoveryOptions& source,
    const std::filesystem::path& config_file) {
  Require(source.backend != DiscoveryBackend::kRegistry,
          "Registry subscription connections come from registry metadata.");
  const std::string driver(ibmw::discovery::BackendName(source.backend));
  const std::string extension_name = driver + "_extension";
  YAML::Node root, extension, transport;
  extension["name"] = extension_name;
  extension["path"] = TransportLibrary(driver).string();
  transport["type"] = driver;
  auto runtime = root["ibmw"];
  const auto resolve_path = [&](const std::string& value) {
    Require(!value.empty(), "Transport configuration path cannot be empty.");
    std::filesystem::path path(value);
    return (path.is_absolute() ? path :
        std::filesystem::absolute(config_file).parent_path() / path).lexically_normal().string();
  };
  if (!config_file.empty()) {
    const auto input = YAML::LoadFile(config_file.string());
    const auto config = input["ibmw"];
    Require(config.IsMap(), "--config-file must contain an ibmw map.");
    const auto items = config["extensions"]["items"];
    Require(items.IsSequence(), "--config-file requires ibmw.extensions.items.");
    unsigned matches = 0;
    for (const auto& item : items) {
      if (item["name"].as<std::string>() != extension_name) continue;
      ++matches;
      extension = YAML::Clone(item);
      extension["path"] = resolve_path(item["path"].as<std::string>());
    }
    Require(matches == 1, "--config-file must contain exactly one " + extension_name + ".");
    matches = 0;
    const auto drivers = config["messaging"]["drivers"];
    Require(drivers.IsSequence(), "--config-file requires ibmw.messaging.drivers.");
    for (const auto& item : drivers) {
      if (item["type"].as<std::string>() != driver) continue;
      ++matches;
      transport = YAML::Clone(item);
    }
    Require(matches == 1, "--config-file must contain exactly one " + driver + " driver.");
    if (config["scheduling"]) runtime["scheduling"] = YAML::Clone(config["scheduling"]);
  }
  auto settings = extension["options"];
  if (!settings || settings.IsNull()) settings = YAML::Node(YAML::NodeType::Map);
  Require(settings.IsMap(), "Transport extension options must be a map.");
  if (source.backend == DiscoveryBackend::kZenoh) {
    if (source.zenoh_config.empty() && settings["native_cfg_path"] &&
        !settings["native_cfg_path"].as<std::string>().empty()) {
      source.zenoh_config = resolve_path(settings["native_cfg_path"].as<std::string>());
    }
    if (!source.zenoh_config.empty()) {
      source.zenoh_config = std::filesystem::absolute(source.zenoh_config);
      settings["native_cfg_path"] = source.zenoh_config.string();
    }
    if (source.zenoh_domain.empty()) settings.remove("limit_domain");
    else settings["limit_domain"] = source.zenoh_domain;
  } else {
    settings["domain_id"] = source.domain_id;
    settings[source.backend == DiscoveryBackend::kDds ? "participant_name" : "node_name"] =
        "ibmw_topic_" + std::to_string(::getpid());
    if (settings["xml_profile_file"] && !settings["xml_profile_file"].as<std::string>().empty()) {
      settings["xml_profile_file"] = resolve_path(settings["xml_profile_file"].as<std::string>());
    }
  }
  extension["options"] = settings;
  runtime["extensions"]["items"].push_back(extension);
  runtime["messaging"]["drivers"].push_back(transport);
  runtime["runtime_graph"]["enabled"] = false;
  runtime["logging"]["core_level"] = "WARN";
  YAML::Node console;
  console["type"] = "console";
  runtime["logging"]["backends"].push_back(console);
  return root;
}

class TopicSubscription::Impl {
 public:
  explicit Impl(Options value) : options(std::move(value)) {}
  ~Impl() {
    try {
      Stop();
    } catch (const std::exception& error) {
      MW_LOG(ibmw::common::util::LoggerWrapper{}, ibmw::common::util::kLogLevelError,
               "Failed to stop topic subscription for '{}': {}.", options.topic, error.what());
    } catch (...) {
      MW_LOG(ibmw::common::util::LoggerWrapper{}, ibmw::common::util::kLogLevelError,
               "Failed to stop topic subscription for '{}' with an unknown error.", options.topic);
    }
  }

  void Start(ibmw::transport::SerializedMessageCallback value) {
    Require(!provider && bool(value), "Subscription is already started or has no callback.");
    callback = std::move(value);
    YAML::Node config;
    if (options.source.backend != DiscoveryBackend::kRegistry) {
      config = SubscriptionRuntimeConfig(options.source, options.config_file);
    }
    provider = ibmw::discovery::CreateDiscoveryProvider(options.source.backend);
    Require(bool(provider), "Selected discovery backend is not available in this installation.");
    provider->Start(options.source);
    if (options.source.backend != DiscoveryBackend::kRegistry) {
      TemporaryConfig file(config);
      engine = std::make_unique<Engine>();
      engine->Initialize({.cfg_file_path = file.Path()});
      auto& bus = engine->GetTransportBus();
      for (const auto* driver : bus.GetUsedTransportDriver()) {
        if (driver->Name() != ibmw::discovery::BackendName(options.source.backend)) continue;
        const auto* capability = bus.GetSerializedTransportCapability(*driver);
        Require(capability && capability->HasValidContract() &&
                    capability->capture_mode == transport::SerializedCaptureMode::kNativeSerialized,
                "Selected transport does not support serialized subscriptions.");
        operations = capability->operations;
      }
      Require(operations.context, "Selected transport driver is unavailable.");
      future = engine->AsyncStart();
    }
  }

  bool Refresh() {
    CheckError();
    Require(bool(provider), "Subscription has not been started.");
    const auto snapshot = provider->Query(std::chrono::steady_clock::now());
    for (const auto& status : snapshot.sources) {
      Require(status.status == ibmw::discovery::DiscoveryStatus::kReady,
              "Topic discovery failed: " + status.message);
    }
    Require(snapshot.Succeeded(), "Topic discovery returned no source status.");
    std::set<std::string> types;
    std::vector<ibmw::discovery::DiscoveredEndpoint> endpoints;
    for (const auto& endpoint : snapshot.endpoints) {
      if (endpoint.topic_name != options.topic &&
          !(endpoint.topic_name.empty() && endpoint.wire_topic_name == options.topic)) continue;
      const auto& type = endpoint.type_name.empty() ? endpoint.wire_type_name : endpoint.type_name;
      if (!options.type.empty() && type != options.type) continue;
      if (endpoint.role != EndpointRole::kPublisher) continue;
      types.insert(type);
      endpoints.push_back(endpoint);
    }
    Require(types.size() <= 1, "Topic '" + options.topic + "' has multiple types; select one with --type.");
    if (!types.empty()) {
      Require(selected_type.empty() || selected_type == *types.begin(),
              "Topic type changed while observing '" + options.topic + "'.");
      selected_type = *types.begin();
    }
    std::set<std::string> wanted;
    if (options.source.backend == DiscoveryBackend::kRegistry) {
      const auto* registry = dynamic_cast<ibmw::discovery::RegistryDiscoveryProvider*>(provider.get());
      Require(registry != nullptr, "Registry discovery has no publisher connection information.");
      std::vector<runtime_graph::TopicAdvertisement> advertisements;
      for (const auto& runtime : registry->Runtimes()) {
        for (auto advertisement : runtime.topics) {
          if (!advertisement.published || advertisement.topic.topic != options.topic ||
              advertisement.topic.msg_type != selected_type) continue;
          advertisement.runtime_id = runtime.runtime_id;
          advertisement.control_socket = runtime.control_socket;
          advertisements.push_back(std::move(advertisement));
        }
      }
      const auto subscriptions = SelectRegistrySubscriptions(advertisements,
          [this](const auto& subscription) { return registry_runtime.Available(subscription); });
      for (const auto& subscription : subscriptions) wanted.insert(RegistrySubscriptionKey(subscription));
      // Stop obsolete routes before attaching replacements, including a switch
      // from one cached subscription to one Tap for each publisher.
      for (auto it = sources.begin(); it != sources.end();) {
        if (wanted.contains(it->first)) { ++it; continue; }
        it->second->Stop();
        it = sources.erase(it);
      }
      for (const auto& subscription : subscriptions) {
        const auto key = RegistrySubscriptionKey(subscription);
        if (sources.contains(key)) continue;
        if (subscription.driver != "runtime-tap") {
          Add(key, registry_runtime.Create(subscription));
          continue;
        }
        auto client = std::make_unique<UnixRuntimeTapClient>(subscription.publisher.control_socket,
            [this](uint64_t count) { dropped.fetch_add(count); },
            [this](const std::string& error) {
              std::lock_guard lock(error_mutex);
              if (stream_error.empty()) stream_error = error;
            });
        Add(key, std::make_unique<RuntimeTapSource>(subscription.publisher, std::move(client)));
      }
      registry_runtime.StartPending();
    } else {
      std::map<std::string, transport::SerializedSubscriberOptionsV2> subscriptions;
      for (const auto& endpoint : endpoints) {
        transport::SerializedSubscriberOptionsV2 subscription;
        subscription.topic.topic = options.topic;
        subscription.wire_topic_name = endpoint.wire_topic_name;
        subscription.wire_type_name = endpoint.wire_type_name;
        if (options.source.backend == DiscoveryBackend::kDds) {
          subscription.topic.msg_type = "dds:" + endpoint.wire_type_name;
          subscription.topic.serialization_type = "cdr";
        } else if (options.source.backend == DiscoveryBackend::kRos2) {
          subscription.wire_topic_name = endpoint.topic_name;
          subscription.wire_type_name = endpoint.type_name;
          subscription.topic.msg_type = "ros2:" + endpoint.type_name;
          subscription.topic.serialization_type = "ros2";
        } else {
          subscription.topic.msg_type = endpoint.type_name;
          subscription.wire_type_name = endpoint.type_name;
        }
        Require(!subscription.wire_topic_name.empty() && !subscription.wire_type_name.empty(),
                "Discovered topic has no subscription wire address.");
        subscription.requested_qos.policies = {
            {"reliability", "best_effort"}, {"durability", "volatile"}, {"liveliness", "automatic"}};
        if (options.source.backend == DiscoveryBackend::kDds) {
          const auto ownership = endpoint.qos.policies.find("ownership");
          subscription.requested_qos.policies["ownership"] =
              ownership == endpoint.qos.policies.end() ? "shared" : ownership->second;
        }
        for (const auto& [name, value] : options.qos.policies) {
          subscription.requested_qos.policies[name] = value;
        }
        if (options.source.backend == DiscoveryBackend::kDds &&
            subscription.requested_qos.policies.at("ownership") == "exclusive") {
          // Fast DDS requires reliable readers for exclusive ownership. Keep
          // explicit reliability choices visible instead of silently overriding them.
          if (!options.qos.policies.contains("reliability"))
            subscription.requested_qos.policies["reliability"] = "reliable";
          Require(subscription.requested_qos.policies.at("reliability") == "reliable",
                  "DDS exclusive ownership is incompatible with best_effort reliability; request reliable QoS.");
        }
        const auto key = subscription.wire_topic_name + '\0' + subscription.wire_type_name;
        auto [it, inserted] = subscriptions.emplace(key, subscription);
        Require(inserted || it->second.requested_qos.policies == subscription.requested_qos.policies,
                "Publishers have incompatible ownership QoS.");
      }
      for (auto& [key, subscription] : subscriptions) {
        if (auto existing = sources.find(key); existing != sources.end()) {
          if (network_qos.at(key).policies == subscription.requested_qos.policies) continue;
          // Ownership is immutable on a DDS reader. Drain the old source before
          // creating its replacement so one wire address has only one reader.
          existing->second->Stop();
          sources.erase(existing);
          network_qos.erase(key);
        }
        Require(operations.supports_topic(operations.context, subscription.topic),
                "Selected transport cannot subscribe to this topic type.");
        const auto requested_qos = subscription.requested_qos;
        Add(key, std::make_unique<TransportSubscriptionSource>(
            std::string(ibmw::discovery::BackendName(options.source.backend)),
            operations, std::move(subscription), engine->GetLogger()));
        network_qos.emplace(key, requested_qos);
      }
      // Keep network readers alive when publishers disappear. They automatically
      // match publishers that subsequently return with the same wire address.
    }
    return !sources.empty();
  }

  void Add(const std::string& key, std::unique_ptr<SerializedMessageSource> source) {
    source->Start(callback);
    sources.emplace(key, std::move(source));
  }

  void CheckError() {
    std::lock_guard lock(error_mutex);
    Require(stream_error.empty(), stream_error);
  }

  void Stop() {
    std::string error;
    for (auto& [key, source] : sources) {
      try { source->Stop(); }
      catch (const std::exception& exception) { if (error.empty()) error = exception.what(); }
    }
    sources.clear();
    network_qos.clear();
    try { registry_runtime.Stop(); }
    catch (const std::exception& exception) { if (error.empty()) error = exception.what(); }
    if (engine) {
      engine->Shutdown();
      if (future.valid()) future.wait();
      engine.reset();
    }
    if (provider) { provider->Stop(); provider.reset(); }
    if (!error.empty()) throw std::runtime_error(error);
    CheckError();
  }

  std::map<std::string, ibmw::transport::QosDescriptor> network_qos;
  Options options;
  ibmw::transport::SerializedMessageCallback callback;
  std::unique_ptr<ibmw::discovery::DiscoveryProvider> provider;
  std::unique_ptr<Engine> engine;
  std::future<void> future;
  transport::SerializedTransportOperationsV2 operations;
  std::mutex error_mutex;
  std::string stream_error;
  std::atomic<uint64_t> dropped{0};
  std::string selected_type;
  RegistrySubscriptionRuntime registry_runtime;
  std::map<std::string, std::unique_ptr<SerializedMessageSource>> sources;
};

TopicSubscription::TopicSubscription(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
TopicSubscription::~TopicSubscription() = default;
void TopicSubscription::Start(ibmw::transport::SerializedMessageCallback callback) {
  impl_->Start(std::move(callback));
}
bool TopicSubscription::Refresh() { return impl_->Refresh(); }
uint64_t TopicSubscription::DroppedMessages() const { return impl_->dropped.load(); }
void TopicSubscription::Stop() { impl_->Stop(); }

}  // namespace ibmw::runtime::core::topic_io
