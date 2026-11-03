// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/runtime_graph_subsystem.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>

#include <unistd.h>

#include "base/exception.h"
#include "engine/engine.h"
#include "engine/extension/extension_runtime.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::runtime_graph::RuntimeGraphSubsystem::Options> {
  using Options = ibmw::runtime::core::runtime_graph::RuntimeGraphSubsystem::Options;

  static Node encode(const Options& value) {
    Node node;
    node["enabled"] = value.enabled;
    node["runtime_id"] = value.runtime_id;
    node["registry_path"] = value.registry_path.string();
    node["type_support_pkgs"] = value.type_support_pkgs;
    return node;
  }

  static bool decode(const Node& node, Options& value) {
    if (!node.IsMap()) return false;
    if (node["enabled"]) value.enabled = node["enabled"].as<bool>();
    if (node["runtime_id"]) value.runtime_id = node["runtime_id"].as<std::string>();
    if (node["registry_path"]) {
      value.registry_path = node["registry_path"].as<std::string>();
    }
    if (node["type_support_pkgs"] && node["type_support_pkgs"].IsSequence()) {
      for (const auto& item : node["type_support_pkgs"]) {
        value.type_support_pkgs.emplace_back(
            item.IsScalar() ? item.as<std::string>()
                             : item["path"].as<std::string>());
      }
    }
    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::runtime_graph {
namespace {

class RuntimePublishTapCapability final : public topic_io::PublishTapCapability {
 public:
  explicit RuntimePublishTapCapability(ibmw::extension::RuntimeV2& runtime)
      : runtime_(runtime) {}

  uint64_t Register(Callback callback) override {
    return runtime_.RegisterPublishTap(std::move(callback));
  }

  bool Unregister(uint64_t handle) override {
    return runtime_.UnregisterPublishTap(handle);
  }

 private:
  ibmw::extension::RuntimeV2& runtime_;
};

bool IsHiddenTopic(std::string_view topic) {
  size_t start = 0;
  while (start < topic.size()) {
    while (start < topic.size() && topic[start] == '/') ++start;
    if (start < topic.size() && topic[start] == '_') return true;
    const auto slash = topic.find('/', start);
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return false;
}

}  // namespace

void RuntimeGraphSubsystem::Configure(Options options) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Runtime Graph can only be configured before initialization.");
  options_ = std::move(options);
  LoadConfiguredTypeSupportPackages();
  configured_ = true;
}

void RuntimeGraphSubsystem::Configure(YAML::Node options_node) {
  explicit_options_ = YAML::Clone(options_node);
  if (options_node && !options_node.IsNull()) {
    Configure(options_node.as<Options>());
  } else {
    Configure(Options{});
  }
}

void RuntimeGraphSubsystem::ApplyFallbackOptions(
    const ibmw::extension::RuntimeGraphOptions& options) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Runtime Graph fallback options require pre-initialization state.");
  if (!explicit_options_["enabled"] && options.enabled) {
    options_.enabled = *options.enabled;
  }
  if (!explicit_options_["runtime_id"] && options.runtime_id) {
    options_.runtime_id = *options.runtime_id;
  }
  if (!explicit_options_["registry_path"] && options.registry_path) {
    options_.registry_path = *options.registry_path;
  }
  std::set<std::string> paths;
  for (const auto& path : options.type_support_pkgs) {
    MW_REQUIRE(paths.emplace(path).second,
               "Duplicate TypeSupport package path {}.", path);
    RegisterTypeSupportPackage(path);
  }
}

void RuntimeGraphSubsystem::LoadConfiguredTypeSupportPackages() {
  std::set<std::string> configured_paths;
  for (const auto& package : options_.type_support_pkgs) {
    MW_REQUIRE(
        configured_paths.emplace(package).second,
        "Duplicate TypeSupport package path {}.", package);
    type_support_registry_.Load(package);
  }
}

void RuntimeGraphSubsystem::RegisterTypeSupportPackage(std::string package_path) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "TypeSupport packages must be registered before initialization.");
  MW_REQUIRE(!package_path.empty(), "TypeSupport package path cannot be empty.");
  type_support_registry_.Load(package_path);
}

ibmw::util::TypeSupportRef RuntimeGraphSubsystem::FindTypeSupport(
    std::string_view msg_type,
    std::string_view package_path) const {
  return type_support_registry_.Find(msg_type, package_path);
}

std::string RuntimeGraphSubsystem::TypeSupportPackage(
    std::string_view msg_type,
    std::string_view package_path) const {
  return type_support_registry_.PackagePath(msg_type, package_path);
}

std::optional<ibmw::util::SchemaDescriptor> RuntimeGraphSubsystem::FindSchema(
    std::string_view msg_type,
    std::string_view serialization_type) const {
  return type_support_registry_.FindSchema(msg_type, serialization_type);
}

void RuntimeGraphSubsystem::HideModulePublishers(std::string module_name) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Hidden publishers must be configured before initialization.");
  hidden_module_publishers_.emplace(std::move(module_name));
}

void RuntimeGraphSubsystem::SetStartTapHandler(RuntimeGraphAgent::StartTapHandler handler) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Runtime Graph handlers must be set before initialization.");
  start_tap_handler_ = std::move(handler);
  if (agent_) agent_->SetStartTapHandler(start_tap_handler_);
}

void RuntimeGraphSubsystem::SetStopTapHandler(RuntimeGraphAgent::StopTapHandler handler) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Runtime Graph handlers must be set before initialization.");
  stop_tap_handler_ = std::move(handler);
  if (agent_) agent_->SetStopTapHandler(stop_tap_handler_);
}

void RuntimeGraphSubsystem::SetStartPlaybackHandler(RuntimeGraphAgent::StartPlaybackHandler handler) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Runtime Graph handlers must be set before initialization.");
  start_playback_handler_ = std::move(handler);
  if (agent_) agent_->SetStartPlaybackHandler(start_playback_handler_);
}

void RuntimeGraphSubsystem::SetStopPlaybackHandler(RuntimeGraphAgent::StopPlaybackHandler handler) {
  MW_REQUIRE(state_.load() == State::kPreInit,
             "Runtime Graph handlers must be set before initialization.");
  stop_playback_handler_ = std::move(handler);
  if (agent_) agent_->SetStopPlaybackHandler(stop_playback_handler_);
}

RuntimeRegistration RuntimeGraphSubsystem::BuildRegistration() const {
  std::array<char, 4096> executable_buffer{};
  const ssize_t executable_size = ::readlink(
      "/proc/self/exe", executable_buffer.data(), executable_buffer.size() - 1);
  const std::string executable_name =
      executable_size > 0
          ? std::filesystem::path(std::string(
                executable_buffer.data(), static_cast<size_t>(executable_size)))
                .filename()
                .string()
          : "ibmw";
  RuntimeRegistration registration{
      .runtime_id = GenerateRuntimeId(options_.runtime_id, executable_name)};

  MW_REQUIRE(engine_, "Runtime Graph Engine is not configured.");
  auto runtime = extension::MakeExtensionRuntimeV2(*engine_);
  for (const auto& endpoint : runtime->GetTransportEndpoints()) {
    if (endpoint.published && hidden_module_publishers_.contains(endpoint.module_name)) {
      continue;
    }
    const std::string serialization_type(
        endpoint.type_support.DefaultSerializationType());
    const auto schema =
        FindSchema(endpoint.msg_type, serialization_type);
    const auto type_support_package =
        TypeSupportPackage(endpoint.msg_type, endpoint.package_path);
    const bool supports_tap = endpoint.published && options_.enabled &&
                              schema.has_value();
    std::map<std::string, TransportCaptureCapability> capabilities;
    for (const auto& [driver, advertised] : endpoint.capture_capabilities) {
      TransportCaptureCapability capability{.mode = "runtime_tap"};
      capability.priority = advertised.priority;
      capability.abi_version = advertised.abi_version;
      const bool cached_supported =
          advertised.mode != ibmw::extension::CaptureMode::kCachedSerialized ||
          !type_support_package.empty();
      if (advertised.supports_topic && cached_supported &&
          !advertised.extension_name.empty() &&
          !advertised.extension_path.empty() &&
          !advertised.driver_options_yaml.empty()) {
        capability.extension_name = advertised.extension_name;
        capability.extension_path = advertised.extension_path;
        capability.extension_options_yaml = advertised.extension_options_yaml;
        capability.driver_options_yaml = advertised.driver_options_yaml;
        capability.provider_instance_option =
            advertised.provider_instance_option;
        capability.provider_clear_option = advertised.provider_clear_option;
        capability.provider_strip_leading_slash_option =
            advertised.provider_strip_leading_slash_option;
        if (advertised.mode ==
            ibmw::extension::CaptureMode::kNativeSerialized) {
          capability.mode = "native_serialized";
        } else if (advertised.mode ==
                   ibmw::extension::CaptureMode::kCachedSerialized) {
          capability.mode = "cached_serialized";
        }
      }
      capabilities.emplace(driver, std::move(capability));
    }
    if (supports_tap) {
      capabilities.emplace("runtime-tap",
                           TransportCaptureCapability{.mode = "runtime_tap"});
    }
    registration.topics.emplace_back(TopicAdvertisement{
        .runtime_id = registration.runtime_id,
        .publisher_id = endpoint.published
                            ? endpoint.module_name + "-" +
                                  std::to_string(endpoint.index)
                            : "subscriber-" + endpoint.module_name + "-" +
                                  std::to_string(endpoint.index),
        .topic =
            {.topic = endpoint.topic,
             .msg_type = endpoint.msg_type,
             .serialization_type = serialization_type,
             .schema = schema.value_or(ibmw::util::SchemaDescriptor{}),
             .qos = endpoint.qos,
             .available_drivers = endpoint.drivers},
        .type_support_package = type_support_package,
        .capture_capabilities = std::move(capabilities),
        .published = endpoint.published,
        .hidden = IsHiddenTopic(endpoint.topic),
        .module_name = endpoint.module_name,
        .driver_qos = endpoint.driver_qos});
  }
  return registration;
}

void RuntimeGraphSubsystem::Initialize(YAML::Node options_node) {
  MW_REQUIRE(std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
             "Runtime Graph can only be initialized once.");
  if (!configured_) {
    if (options_node && !options_node.IsNull()) {
      options_ = options_node.as<Options>();
    }
    LoadConfiguredTypeSupportPackages();
    configured_ = true;
  }
  if (options_.registry_path.empty()) {
    options_.registry_path = ibmw::extension::DefaultRuntimeRegistryPath();
  }
  DiscoverTypeSupportPackagesFromCatalog();
  registration_ = BuildRegistration();
  RefreshModuleList();
  RefreshServiceList();
  if (!options_.enabled) return;
  tap_runtime_ = extension::MakeExtensionRuntimeV2(*engine_);
  publish_tap_ = std::make_unique<RuntimePublishTapCapability>(*tap_runtime_);
  tap_server_ = std::make_unique<topic_io::RuntimeTapServer>(
      *publish_tap_, registration_.runtime_id, options_.registry_path, GetLogger());
  tap_server_->Start();
  agent_ = std::make_unique<RuntimeGraphAgent>(RuntimeGraphAgent::Options{
      .registration = registration_, .registry_path = options_.registry_path,
      .logger = GetLogger()});
  agent_->SetListModuleLifecycleHandler([this] {
    return engine_->GetModuleBus().GetModuleLifecycleStatuses();
  });
  agent_->SetStartTapHandler(start_tap_handler_ ? start_tap_handler_ :
      RuntimeGraphAgent::StartTapHandler([this](const auto& advertisement) {
        return tap_server_->StartSession(advertisement);
      }));
  agent_->SetStopTapHandler(stop_tap_handler_ ? stop_tap_handler_ :
      RuntimeGraphAgent::StopTapHandler([this](auto session_id) {
        return tap_server_->StopSession(session_id);
      }));
  if (start_playback_handler_) {
    agent_->SetStartPlaybackHandler(start_playback_handler_);
  }
  if (stop_playback_handler_) {
    agent_->SetStopPlaybackHandler(stop_playback_handler_);
  }
  agent_->Initialize();
  registration_ = agent_->Registration();
}

void RuntimeGraphSubsystem::RefreshModuleList() {
  MW_REQUIRE(state_.load() == State::kInit && engine_,
             "Module registration requires an initialized Runtime Graph.");
  std::vector<std::string> modules;
  for (const auto* info : engine_->GetModuleBus().GetModuleDetailInfoList()) {
    modules.push_back(info->module_name);
  }
  std::sort(modules.begin(), modules.end());
  if (registration_.modules && *registration_.modules == modules) return;
  auto updated = registration_;
  updated.modules = std::move(modules);
  if (agent_) agent_->UpdateRegistration(updated);
  registration_ = std::move(updated);
}

void RuntimeGraphSubsystem::RefreshServiceList() {
  MW_REQUIRE(state_.load() == State::kInit && engine_,
             "Service registration requires an initialized Runtime Graph.");
  const auto& dispatcher = engine_->GetServiceDispatcher();
  const auto* catalog = dispatcher.GetServiceCatalog();
  MW_REQUIRE(catalog, "Service catalog is not initialized.");
  const auto& drivers = dispatcher.GetServiceDriverDispatcher();
  std::vector<ibmw::extension::ServiceAdvertisement> services;
  const auto append = [&](const auto& wrappers, const auto& driver_map, bool server) {
    for (const auto& [key, wrapper] : wrappers) {
      const auto& info = wrapper->info;
      ibmw::extension::ServiceAdvertisement service{
          .endpoint_id = std::string(server ? "server-" : "client-") + std::to_string(info.index),
          .service_name = info.func_name,
          .request_type = std::string(info.req_type_support_ref.TypeName()),
          .response_type = std::string(info.rsp_type_support_ref.TypeName()),
          .module_name = info.module_name,
          .package_path = info.pkg_path,
          .server = server,
          .hidden = IsHiddenTopic(info.func_name)};
      if (const auto found = driver_map.find(info.func_name); found != driver_map.end()) {
        for (const auto driver : found->second) service.available_drivers.emplace_back(driver);
      }
      services.push_back(std::move(service));
    }
  };
  append(catalog->GetServiceFuncWrapperMap(), drivers.GetServersDriverInfo(), true);
  append(catalog->GetClientFuncWrapperMap(), drivers.GetClientsDriverInfo(), false);
  std::sort(services.begin(), services.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.endpoint_id < rhs.endpoint_id;
  });
  if (registration_.services && *registration_.services == services) return;
  auto updated = registration_;
  updated.services = std::move(services);
  if (agent_) agent_->UpdateRegistration(updated);
  registration_ = std::move(updated);
}

void RuntimeGraphSubsystem::DiscoverTypeSupportPackagesFromCatalog() {
  MW_REQUIRE(engine_, "Runtime Graph Engine is not configured.");
  auto runtime = extension::MakeExtensionRuntimeV2(*engine_);
  std::set<std::string> package_paths;
  for (const auto& endpoint : runtime->GetTransportEndpoints()) {
    if (!endpoint.package_path.empty() && endpoint.package_path != "core") {
      package_paths.emplace(endpoint.package_path);
    }
  }
  for (const auto& package_path : package_paths) {
    if (!type_support_registry_.TryLoad(package_path)) {
      MW_WARN("Module package '{}' has no loadable TypeSupport package.", package_path);
    }
  }
}

void RuntimeGraphSubsystem::Start() {
  MW_REQUIRE(std::atomic_exchange(&state_, State::kStart) == State::kInit,
             "Runtime Graph must be initialized before Start.");
  if (agent_) agent_->Start();
}

void RuntimeGraphSubsystem::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown) return;
  if (agent_) agent_->Shutdown();
  agent_.reset();
  if (tap_server_) tap_server_->Shutdown();
  tap_server_.reset();
  publish_tap_.reset();
  tap_runtime_.reset();
  // Destroy extension-owned callbacks while their libraries are still loaded.
  start_tap_handler_ = {};
  stop_tap_handler_ = {};
  start_playback_handler_ = {};
  stop_playback_handler_ = {};
}

const RuntimeRegistration& RuntimeGraphSubsystem::Registration() const {
  MW_REQUIRE(state_.load() != State::kPreInit,
             "Runtime Graph registration is unavailable before initialization.");
  return agent_ ? agent_->Registration() : registration_;
}

}  // namespace ibmw::runtime::core::runtime_graph
