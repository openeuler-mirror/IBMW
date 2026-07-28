// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/record_playback_extension.h"

#include <chrono>
#include <array>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

#include <unistd.h>

#include "cpp/service/rpc_handle.h"
#include "cpp/executor/executor.h"
#include "cpp/util/type_support.h"
#include "extension_api/extension_logger.h"
#include "extension_api/ibmw_extension_main.h"
#include "extension_api/ibmw_extension_runtime.h"
#include "extension_api/type_support_package_loader.h"
#include "record_playback_ext/global.h"
#include "base/time_util.h"
#include "record_playback_ext/playback_action.h"
#include "record_playback_ext/record_action.h"
#include "record_playback_ext/schema/schema_registry.h"
#include "record_playback_ext/service.h"
#include "record_playback_ext/runtime/bag_runtime_agent.h"
#include "record_playback_ext/runtime/runtime_playback_stream.h"
#include "record_playback_ext/runtime/runtime_tap_stream.h"

namespace ibmw::extensions::record_playback_extension {

class RecordPlaybackExtension : public ibmw::EngineExtensionBase {
 public:
  struct Options {
    std::string service_name;
    struct TypeSupportPkg { std::string path; };
    std::vector<TypeSupportPkg> type_support_pkgs;
    std::string timer_executor;
    bool enable_runtime_agent = true;
    std::string runtime_id;
    std::string runtime_registry_path = "/tmp/ibmw/runtimes";
    struct RecordActionOptions { std::string name; YAML::Node options; };
    std::vector<RecordActionOptions> record_actions;
    struct PlaybackActionOptions { std::string name; YAML::Node options; };
    std::vector<PlaybackActionOptions> playback_actions;
  };

  RecordPlaybackExtension();
  ~RecordPlaybackExtension() override = default;
  std::string_view Name() const noexcept override {
    return "record_playback_extension";
  }
  bool Initialize(runtime::core::Engine*) noexcept override { return false; }
  uint32_t RequiredRuntimeAbi() const noexcept override {
    return ibmw::extension::kRuntimeAbiVersionV1;
  }
  bool InitializeRuntime(ibmw::extension::RuntimeV1* runtime) noexcept override;
  void Shutdown() noexcept override;

 private:
  void InitTypeSupport(Options::TypeSupportPkg& options);
  bool TryInitTypeSupport(
      std::string_view package_path,
      std::string_view owner_package_path = {});
  void RegisterTypeSupportPackage(
      std::string package_path,
      std::unique_ptr<ibmw::extension::TypeSupportPackageLoader> loader);
  void DiscoverTypeSupportPackagesFromCatalog();
  void RegisterRpcService();
  void RegisterRecordTransport();
  void RegisterPlaybackTransport();
  void RegisterRuntimePlaybackTransport();
  void PublishRuntimePlaybackMessage(
      const TopicAdvertisement& advertisement,
      ibmw::transport::SerializedMessage&& message);
  void StartRuntimeAgent();
  void StopRuntimeAgent();
  RuntimeRegistration BuildRuntimeRegistration() const;

  Options options_;
  ibmw::extension::RuntimeV1* runtime_ = nullptr;
  bool initialized_ = false;
  std::unique_ptr<RecordPlaybackServiceImpl> service_ptr_;
  std::vector<std::unique_ptr<ibmw::extension::TypeSupportPackageLoader>>
      type_support_pkg_loader_vec_;
  ibmw::executor::ExecutorHandle timer_executor_ref_;

  struct TypeSupportWrapper {
    std::string package_path;
    ibmw::util::TypeSupportRef type_support_ref;
    ibmw::extension::TypeSupportPackageLoader* loader_ptr;
  };
  static std::string PackageTypeKey(
      std::string_view package_path, std::string_view msg_type);
  const TypeSupportWrapper* FindTypeSupport(
      std::string_view msg_type, std::string_view package_path = {}) const;
  std::unordered_map<std::string, TypeSupportWrapper> type_support_map_;
  std::unordered_map<std::string, TypeSupportWrapper>
      package_type_support_map_;
  std::unordered_map<std::string, std::string>
      module_type_support_package_map_;
  std::unordered_set<std::string> loaded_type_support_packages_;
  SchemaRegistry schema_registry_;
  std::unique_ptr<RuntimeTapServer> runtime_tap_server_;
  std::unique_ptr<PublishTapCapability> publish_tap_capability_;
  std::unique_ptr<RuntimePlaybackServer> runtime_playback_server_;
  std::unique_ptr<BagRuntimeAgent> runtime_agent_;
  std::unordered_map<std::string, ibmw::extension::PublisherHandle>
      runtime_playback_publishers_;
  std::unordered_map<std::string_view, std::unique_ptr<RecordAction>>
      record_action_map_;
  std::unordered_map<std::string_view, std::unique_ptr<PlaybackAction>>
      playback_action_map_;
};

}  // namespace ibmw::extensions::record_playback_extension

namespace YAML {

template <>
struct convert<ibmw::extensions::record_playback_extension::RecordPlaybackExtension::Options> {
  using Options = ibmw::extensions::record_playback_extension::RecordPlaybackExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["service_name"] = rhs.service_name;
    node["timer_executor"] = rhs.timer_executor;
    node["enable_runtime_agent"] = rhs.enable_runtime_agent;
    node["runtime_id"] = rhs.runtime_id;
    node["runtime_registry_path"] = rhs.runtime_registry_path;

    node["type_support_pkgs"] = YAML::Node();
    for (const auto& type_support_pkg : rhs.type_support_pkgs) {
      Node type_support_pkg_node;
      type_support_pkg_node["path"] = type_support_pkg.path;
      node["type_support_pkgs"].push_back(type_support_pkg_node);
    }

    node["record_actions"] = YAML::Node();
    for (const auto& record_action : rhs.record_actions) {
      Node record_action_node;
      record_action_node["name"] = record_action.name;
      record_action_node["options"] = record_action.options;
      node["record_actions"].push_back(record_action_node);
    }

    node["playback_actions"] = YAML::Node();
    for (const auto& playback_action : rhs.playback_actions) {
      Node playback_action_node;
      playback_action_node["name"] = playback_action.name;
      playback_action_node["options"] = playback_action.options;
      node["playback_actions"].push_back(playback_action_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["service_name"])
      rhs.service_name = node["service_name"].as<std::string>();

    if (node["type_support_pkgs"] && node["type_support_pkgs"].IsSequence()) {
      for (const auto& type_support_pkg_node : node["type_support_pkgs"]) {
        auto type_support_pkg = Options::TypeSupportPkg{
            .path = type_support_pkg_node["path"].as<std::string>()};

        rhs.type_support_pkgs.emplace_back(std::move(type_support_pkg));
      }
    }

    if (node["timer_executor"])
      rhs.timer_executor = node["timer_executor"].as<std::string>();
    if (node["enable_runtime_agent"])
      rhs.enable_runtime_agent = node["enable_runtime_agent"].as<bool>();
    if (node["runtime_id"])
      rhs.runtime_id = node["runtime_id"].as<std::string>();
    if (node["runtime_registry_path"])
      rhs.runtime_registry_path =
          node["runtime_registry_path"].as<std::string>();

    if (node["record_actions"] && node["record_actions"].IsSequence()) {
      for (const auto& record_action_node : node["record_actions"]) {
        auto record_action = Options::RecordActionOptions{
            .name = record_action_node["name"].as<std::string>(),
            .options = record_action_node["options"]};

        rhs.record_actions.emplace_back(std::move(record_action));
      }
    }

    if (node["playback_actions"] && node["playback_actions"].IsSequence()) {
      for (const auto& playback_action_node : node["playback_actions"]) {
        auto playback_action = Options::PlaybackActionOptions{
            .name = playback_action_node["name"].as<std::string>(),
            .options = playback_action_node["options"]};

        rhs.playback_actions.emplace_back(std::move(playback_action));
      }
    }
    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::record_playback_extension {

namespace {

constexpr std::string_view kRuntimePlaybackModule = "ibmw-bag-agent";

std::string TypeSupportCompanionPath(std::string_view package_path) {
  std::filesystem::path path(package_path);
  const auto stem = path.stem().string();
  if (path.filename().empty() || stem.empty()) {
    return {};
  }
  path.replace_filename(stem + "_ts" + path.extension().string());
  return path.string();
}

class RuntimePublishTapCapability final : public PublishTapCapability {
 public:
  explicit RuntimePublishTapCapability(ibmw::extension::RuntimeV1& runtime)
      : runtime_(runtime) {}

  uint64_t Register(Callback callback) override {
    return runtime_.RegisterPublishTap(std::move(callback));
  }

  bool Unregister(uint64_t handle) override {
    return runtime_.UnregisterPublishTap(handle);
  }

 private:
  ibmw::extension::RuntimeV1& runtime_;
};

bool IsHiddenTopic(std::string_view topic) {
  size_t segment_start = 0;
  while (segment_start < topic.size()) {
    while (segment_start < topic.size() && topic[segment_start] == '/') {
      ++segment_start;
    }
    if (segment_start < topic.size() && topic[segment_start] == '_') {
      return true;
    }
    const auto separator = topic.find('/', segment_start);
    if (separator == std::string_view::npos) {
      break;
    }
    segment_start = separator + 1;
  }
  return false;
}

std::string PlaybackPublisherKey(
    std::string_view topic,
    std::string_view msg_type) {
  return std::string(topic) + '\0' + std::string(msg_type);
}

void EnsureRuntimePlaybackPublisherRules(ibmw::extension::RuntimeV1& runtime) {
  auto messaging = runtime.GetRuntimeOptions("messaging");
  if (!messaging || !messaging["subscribers"] ||
      !messaging["subscribers"].IsSequence()) {
    return;
  }

  YAML::Node publishers = messaging["publishers"];
  if (!publishers || !publishers.IsSequence()) {
    publishers = YAML::Node(YAML::NodeType::Sequence);
  }
  for (const auto& subscriber : messaging["subscribers"]) {
    if (!subscriber["topic"] || !subscriber["enable_drivers"]) {
      continue;
    }
    const std::string topic = subscriber["topic"].as<std::string>();
    const auto existing = std::find_if(
        publishers.begin(), publishers.end(), [&](const auto& publisher) {
          return publisher["topic"] &&
                 publisher["topic"].template as<std::string>() == topic;
        });
    if (existing != publishers.end()) {
      continue;
    }
    YAML::Node publisher;
    publisher["topic"] = topic;
    publisher["enable_drivers"] = YAML::Clone(subscriber["enable_drivers"]);
    if (subscriber["enable_filters"]) {
      publisher["enable_filters"] =
          YAML::Clone(subscriber["enable_filters"]);
    }
    publishers.push_back(std::move(publisher));
  }
  messaging["publishers"] = std::move(publishers);
}

}  // namespace

RecordPlaybackExtension::RecordPlaybackExtension() = default;

bool RecordPlaybackExtension::InitializeRuntime(
    ibmw::extension::RuntimeV1* runtime) noexcept {
  try {
    MW_REQUIRE(
        runtime != nullptr &&
            runtime->AbiVersion() == ibmw::extension::kRuntimeAbiVersionV1,
        "Record/playback requires Runtime capability V1.");
    runtime_ = runtime;
    YAML::Node extension_options_node = runtime_->GetExtensionOptions(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }
  if (options_.enable_runtime_agent) {
    EnsureRuntimePlaybackPublisherRules(*runtime_);
  }

  // type support
  for (auto& type_support_pkg : options_.type_support_pkgs) {
    // Check for duplicate pkg
    auto finditr = std::find_if(
        options_.type_support_pkgs.begin(), options_.type_support_pkgs.end(),
        [&type_support_pkg](const auto& op) {
          if (&type_support_pkg == &op) return false;
          return op.path == type_support_pkg.path;
        });
    MW_REQUIRE(finditr == options_.type_support_pkgs.end(),
                            "Duplicate pkg path {}", type_support_pkg.path);

    InitTypeSupport(type_support_pkg);
  }

  MW_TRACE("Load {} pkg and {} type.",
              type_support_pkg_loader_vec_.size(), type_support_map_.size());

  // record
  for (auto& record_action_options : options_.record_actions) {
    // Check duplicate record
    auto finditr = std::find_if(
        options_.record_actions.begin(), options_.record_actions.end(),
        [&record_action_options](const auto& op) {
          if (&record_action_options == &op) return false;
          return op.name == record_action_options.name;
        });
    MW_REQUIRE(finditr == options_.record_actions.end(),
                            "Duplicate record action {}", record_action_options.name);

    auto action_ptr = std::make_unique<RecordAction>();

    action_ptr->RegisterGetExecutorFunc(
        [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
          return runtime_->GetExecutor(executor_name);
        });
    action_ptr->RegisterGetTypeSupportFunc(
        [this](std::string_view msg_type) -> ibmw::util::TypeSupportRef {
          if (const auto* type_support = FindTypeSupport(msg_type)) {
            return type_support->type_support_ref;
          }
          return {};
        });
    action_ptr->RegisterGetSchemaSupportFunc(
        [this](std::string_view msg_type, std::string_view serialization_type) {
          return schema_registry_.Find(msg_type, serialization_type);
        });
    action_ptr->Initialize(record_action_options.options);

    record_action_map_.emplace(record_action_options.name, std::move(action_ptr));
  }

  // playback
  for (auto& playback_action_options : options_.playback_actions) {
    // Check for duplicate playback
    auto finditr = std::find_if(
        options_.playback_actions.begin(), options_.playback_actions.end(),
        [&playback_action_options](const auto& op) {
          if (&playback_action_options == &op) return false;
          return op.name == playback_action_options.name;
        });
    MW_REQUIRE(finditr == options_.playback_actions.end(),
                            "Duplicate playback action {}", playback_action_options.name);

    auto action_ptr = std::make_unique<PlaybackAction>();

    action_ptr->RegisterGetExecutorFunc(
        [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
          return runtime_->GetExecutor(executor_name);
        });
    action_ptr->RegisterGetTypeSupportFunc(
        [this](std::string_view msg_type) -> ibmw::util::TypeSupportRef {
          if (const auto* type_support = FindTypeSupport(msg_type)) {
            return type_support->type_support_ref;
          }
          return {};
        });
    action_ptr->Initialize(playback_action_options.options);

    playback_action_map_.emplace(playback_action_options.name, std::move(action_ptr));
  }

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kInitialize,
       ibmw::extension::RuntimeSubsystem::kModule,
       ibmw::extension::RuntimeHookTiming::kBefore},
      [this] {
        RegisterRpcService();
        RegisterRecordTransport();
        RegisterPlaybackTransport();
      });

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kInitialize,
       ibmw::extension::RuntimeSubsystem::kExecutor,
       ibmw::extension::RuntimeHookTiming::kAfter},
      [this] {
        if (record_action_map_.size() != 0) {
          timer_executor_ref_ = runtime_->GetExecutor(options_.timer_executor);
          MW_REQUIRE(timer_executor_ref_,
                                  "Can not get executor {}!", options_.timer_executor);
          MW_REQUIRE(timer_executor_ref_.SupportsTimedDispatch(),
                                  "Executor {} didn't support TimerSchedule!", options_.timer_executor);
        }
        for (auto& itr : record_action_map_) {
          itr.second->InitExecutor(timer_executor_ref_);
        }
        for (auto& itr : playback_action_map_) {
          itr.second->InitExecutor();
        }
      });

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kInitialize,
       ibmw::extension::RuntimeSubsystem::kModule,
       ibmw::extension::RuntimeHookTiming::kAfter},
      [this] { StartRuntimeAgent(); });

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kStart,
       ibmw::extension::RuntimeSubsystem::kAll,
       ibmw::extension::RuntimeHookTiming::kAfter},
      [this] {
        for (auto& itr : record_action_map_) {
          itr.second->Start();
        }
        for (auto& itr : playback_action_map_) {
          itr.second->Start();
        }
      });

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kStopping,
       ibmw::extension::RuntimeSubsystem::kAll,
       ibmw::extension::RuntimeHookTiming::kBefore},
      [this] {
        StopRuntimeAgent();
        for (auto& itr : playback_action_map_)
          itr.second->Shutdown();

        for (auto& itr : record_action_map_)
          itr.second->Shutdown();
      });

  extension_options_node = options_;
  runtime_->UpdateExtensionOptions(Name(), extension_options_node);

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kInitialize,
       ibmw::extension::RuntimeSubsystem::kLogger,
       ibmw::extension::RuntimeHookTiming::kAfter},
      [this] { g_logger.Set(runtime_->GetLogger()); });
  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kStopping,
       ibmw::extension::RuntimeSubsystem::kLogger,
       ibmw::extension::RuntimeHookTiming::kBefore},
      [] { g_logger.Set({}); });

  initialized_ = true;
  return true;
  } catch (const std::exception& error) {
    MW_ERROR("Record/playback initialization failed: {}", error.what());
    return false;
  }
}

void RecordPlaybackExtension::Shutdown() noexcept {
  if (!initialized_) {
    return;
  }
  try {
    StopRuntimeAgent();

    for (auto& itr : playback_action_map_)
      itr.second->Shutdown();

    for (auto& itr : record_action_map_)
      itr.second->Shutdown();
  } catch (const std::exception& error) {
    MW_ERROR("Record/playback shutdown failed: {}", error.what());
  }
}

RuntimeRegistration RecordPlaybackExtension::BuildRuntimeRegistration() const {
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

  for (const auto& endpoint : runtime_->GetTransportEndpoints()) {
    if (endpoint.published && endpoint.module_name == kRuntimePlaybackModule) {
      continue;
    }
    const std::string serialization_type(
        endpoint.type_support.DefaultSerializationType());
    if (serialization_type.empty()) {
      MW_WARN(
          "Runtime Agent skips topic '{}' type '{}' because its TypeSupport "
          "does not advertise a serialization format.",
          endpoint.topic, endpoint.msg_type);
      continue;
    }
    const auto schema =
        schema_registry_.Find(endpoint.msg_type, serialization_type);
    if (!schema) {
      if (endpoint.published) {
        MW_WARN(
            "Runtime Agent skips topic '{}' type '{}' because Schema '{}' is unavailable.",
            endpoint.topic, endpoint.msg_type, serialization_type);
      }
      continue;
    }
    std::string type_support_package;
    if (const auto* type_support = FindTypeSupport(
            endpoint.msg_type, endpoint.package_path)) {
      type_support_package = type_support->package_path;
    }
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
             .schema = *schema,
             .qos = endpoint.qos,
             .available_drivers = endpoint.drivers},
        .type_support_package = type_support_package,
        .capture_capabilities = std::move(capabilities),
        .published = endpoint.published,
        .embedded_tap = endpoint.published,
        .hidden = IsHiddenTopic(endpoint.topic)});
  }
  return registration;
}

void RecordPlaybackExtension::StartRuntimeAgent() {
  if (!options_.enable_runtime_agent || runtime_agent_) {
    return;
  }
  DiscoverTypeSupportPackagesFromCatalog();
  RegisterRuntimePlaybackTransport();
  auto registration = BuildRuntimeRegistration();
  publish_tap_capability_ =
      std::make_unique<RuntimePublishTapCapability>(*runtime_);
  runtime_tap_server_ = std::make_unique<RuntimeTapServer>(
      *publish_tap_capability_, registration.runtime_id,
      options_.runtime_registry_path);
  runtime_tap_server_->Start();
  runtime_playback_server_ = std::make_unique<RuntimePlaybackServer>(
      registration.runtime_id, options_.runtime_registry_path,
      [this](const TopicAdvertisement& advertisement, auto&& message) {
        PublishRuntimePlaybackMessage(
            advertisement, std::move(message));
      });
  runtime_playback_server_->Start();

  runtime_agent_ = std::make_unique<BagRuntimeAgent>(
      BagRuntimeAgent::Options{
          .registration = std::move(registration),
          .registry_path = options_.runtime_registry_path});
  runtime_agent_->SetStartTapHandler(
      [this](const TopicAdvertisement& advertisement) {
        return runtime_tap_server_->StartSession(advertisement);
      });
  runtime_agent_->SetStopTapHandler(
      [this](std::string_view session_id) {
        return runtime_tap_server_->StopSession(session_id);
      });
  runtime_agent_->SetStartPlaybackHandler(
      [this](const TopicAdvertisement& advertisement) {
        return runtime_playback_server_->StartSession(advertisement);
      });
  runtime_agent_->SetStopPlaybackHandler(
      [this](std::string_view session_id) {
        return runtime_playback_server_->StopSession(session_id);
      });
  runtime_agent_->Initialize();
  runtime_agent_->Start();
  MW_INFO(
      "Bag Runtime Agent '{}' advertises {} topic endpoint(s).",
      runtime_agent_->Registration().runtime_id,
      runtime_agent_->Registration().topics.size());
}

void RecordPlaybackExtension::StopRuntimeAgent() {
  if (runtime_agent_) {
    runtime_agent_->Shutdown();
    runtime_agent_.reset();
  }
  if (runtime_tap_server_) {
    runtime_tap_server_->Shutdown();
    runtime_tap_server_.reset();
  }
  publish_tap_capability_.reset();
  if (runtime_playback_server_) {
    runtime_playback_server_->Shutdown();
    runtime_playback_server_.reset();
  }
}

void RecordPlaybackExtension::InitTypeSupport(Options::TypeSupportPkg& options) {
  auto loader =
      std::make_unique<ibmw::extension::TypeSupportPackageLoader>();
  loader->Load(options.path);
  options.path = loader->ResolvedPath();
  MW_REQUIRE(
      loaded_type_support_packages_.emplace(options.path).second,
      "Duplicate TypeSupport package path {}", options.path);
  RegisterTypeSupportPackage(options.path, std::move(loader));
}

bool RecordPlaybackExtension::TryInitTypeSupport(
    std::string_view package_path,
    std::string_view owner_package_path) {
  if (loaded_type_support_packages_.contains(std::string(package_path))) {
    if (!owner_package_path.empty()) {
      module_type_support_package_map_.insert_or_assign(
          std::string(owner_package_path), std::string(package_path));
    }
    return true;
  }
  auto loader =
      std::make_unique<ibmw::extension::TypeSupportPackageLoader>();
  if (!loader->TryLoad(package_path)) {
    return false;
  }
  std::string resolved_path = loader->ResolvedPath();
  if (!loaded_type_support_packages_.emplace(resolved_path).second) {
    loader->Unload();
    if (!owner_package_path.empty()) {
      module_type_support_package_map_.insert_or_assign(
          std::string(owner_package_path), resolved_path);
    }
    return true;
  }
  RegisterTypeSupportPackage(resolved_path, std::move(loader));
  if (!owner_package_path.empty()) {
    module_type_support_package_map_.insert_or_assign(
        std::string(owner_package_path), resolved_path);
  }
  return true;
}

void RecordPlaybackExtension::RegisterTypeSupportPackage(
    std::string package_path,
    std::unique_ptr<ibmw::extension::TypeSupportPackageLoader> loader) {
  const auto type_support_array = loader->TypeSupports();
  const auto schema_support_array = loader->SchemaSupports();

  for (const auto* item : type_support_array) {
    ibmw::util::TypeSupportRef type_support_ref(item);
    const std::string type_name(type_support_ref.TypeName());
    TypeSupportWrapper wrapper{
        .package_path = package_path,
        .type_support_ref = type_support_ref,
        .loader_ptr = loader.get()};
    MW_REQUIRE(
        package_type_support_map_
            .emplace(PackageTypeKey(package_path, type_name), wrapper)
            .second,
        "Duplicate msg type '{}' in TypeSupport package {}.",
        type_name, package_path);
    const auto [fallback, inserted] =
        type_support_map_.emplace(type_name, std::move(wrapper));
    if (!inserted) {
      MW_WARN(
          "Duplicate msg type '{}' in {} and {}; package-local matching will "
          "be used.",
          type_name, package_path, fallback->second.package_path);
    }
  }

  for (const auto* item : schema_support_array) {
    schema_registry_.Register(ibmw::util::SchemaSupportRef(item));
  }

  type_support_pkg_loader_vec_.emplace_back(std::move(loader));
}

void RecordPlaybackExtension::DiscoverTypeSupportPackagesFromCatalog() {
  std::set<std::string> package_paths;
  for (const auto& endpoint : runtime_->GetTransportEndpoints()) {
    if (!endpoint.package_path.empty() && endpoint.package_path != "core") {
      package_paths.emplace(endpoint.package_path);
    }
  }

  for (const auto& package_path : package_paths) {
    bool loaded = TryInitTypeSupport(package_path);
    if (!loaded) {
      const auto companion_path = TypeSupportCompanionPath(package_path);
      if (!companion_path.empty() && companion_path != package_path) {
        loaded = TryInitTypeSupport(companion_path, package_path);
      }
    }
    if (!loaded) {
      MW_WARN(
          "Module package '{}' has no discoverable TypeSupport DSO; Runtime "
          "Agent cannot advertise its Topics unless a matching TypeSupport "
          "package is configured.",
          package_path);
    }
  }
}

std::string RecordPlaybackExtension::PackageTypeKey(
    std::string_view package_path,
    std::string_view msg_type) {
  std::string key(package_path);
  key.push_back('\0');
  key.append(msg_type.data(), msg_type.size());
  return key;
}

const RecordPlaybackExtension::TypeSupportWrapper*
RecordPlaybackExtension::FindTypeSupport(
    std::string_view msg_type,
    std::string_view package_path) const {
  if (!package_path.empty()) {
    const auto exact = package_type_support_map_.find(
        PackageTypeKey(package_path, msg_type));
    if (exact != package_type_support_map_.end()) {
      return &exact->second;
    }
    const auto companion =
        module_type_support_package_map_.find(std::string(package_path));
    if (companion != module_type_support_package_map_.end()) {
      exact = package_type_support_map_.find(
          PackageTypeKey(companion->second, msg_type));
      if (exact != package_type_support_map_.end()) {
        return &exact->second;
      }
    }
  }
  const auto fallback = type_support_map_.find(std::string(msg_type));
  return fallback == type_support_map_.end() ? nullptr : &fallback->second;
}

void RecordPlaybackExtension::RegisterRpcService() {
  service_ptr_ = std::make_unique<RecordPlaybackServiceImpl>();

  if (!options_.service_name.empty())
    service_ptr_->SetServiceName(options_.service_name);

  service_ptr_->SetRecordActionMap(&record_action_map_);
  service_ptr_->SetPlaybackActionMap(&playback_action_map_);

  auto service_ref = ibmw::rpc::ServiceRef(
      runtime_->GetRpcHandle());

  bool ret = service_ref.RegisterService(service_ptr_.get());
  MW_CHECK_ERROR(ret, "Register service failed.");
}

void RecordPlaybackExtension::RegisterRecordTransport() {
  using RecordFunc = std::function<void(uint64_t, ibmw::extension::MessageView&)>;

  struct Wrapper {
    std::unordered_set<std::string> require_cache_serialization_types;
    std::vector<RecordFunc> record_func_vec;
  };

  std::unordered_map<ibmw::transport::TopicMetaKey, Wrapper,
                     ibmw::transport::TopicMetaKey::Hash>
      recore_func_map;

  for (auto& record_action_itr : record_action_map_) {
    auto& record_action = *(record_action_itr.second);

    const auto& topic_meta_map = record_action.GetTopicMetaMap();

    // RecordFunc
    for (const auto& topic_meta_itr : topic_meta_map) {
      const auto& topic_meta = topic_meta_itr.second;

      RecordFunc record_func =
          [&record_action, topic_id{topic_meta.id}, serialization_type{topic_meta.serialization_type}](
              uint64_t cur_timestamp, ibmw::extension::MessageView& msg_wrapper) {
            auto buffer_view_ptr =
                msg_wrapper.acquire_serialized(serialization_type);
            if (!buffer_view_ptr) [[unlikely]] {
              MW_WARN("Can not serialize msg type '{}' with serialization type '{}'.",
                         msg_wrapper.msg_type, serialization_type);
              return;
            }
            auto pub_ts = std::string(msg_wrapper.context.GetMetaValue(
                IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP));
            auto pub_seq = std::string(msg_wrapper.context.GetMetaValue(
                IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ));

            record_action.AddRecord(
                RecordAction::OneRecord{
                    .topic_index = topic_id,
                    .log_timestamp = cur_timestamp,
                    .pub_timestamp = pub_ts.empty() ? 0 : std::stoull(pub_ts),
                    .pub_sequence = pub_seq.empty() ? 0 : static_cast<uint32_t>(std::stoul(pub_seq)),
                    .buffer_view_ptr = buffer_view_ptr});
          };

      auto& item = recore_func_map[topic_meta_itr.first];
      item.require_cache_serialization_types.emplace(topic_meta.serialization_type);
      item.record_func_vec.emplace_back(std::move(record_func));
    }
  }

  // Subscribe
  for (auto& recore_func_itr : recore_func_map) {
    const auto& key = recore_func_itr.first;
    auto& wrapper = recore_func_itr.second;

    const auto* type_support_wrapper = FindTypeSupport(key.msg_type);
    MW_REQUIRE(
        type_support_wrapper != nullptr,
        "Cannot find TypeSupport for record type '{}'.", key.msg_type);
    auto type_support_ref = type_support_wrapper->type_support_ref;

    // Small optimization
    auto& record_func_vec = wrapper.record_func_vec;
    ibmw::extension::SubscriptionOptions subscription{
        .topic = key.topic_name,
        .msg_type = key.msg_type,
        .package_path = type_support_wrapper->package_path,
        .module_name = "core",
        .type_support = type_support_ref,
        .required_serialization_types =
            wrapper.require_cache_serialization_types};
    ibmw::extension::RuntimeV1::MessageCallback callback;
    if (record_func_vec.size() == 1) {
      callback =
          [record_func{std::move(record_func_vec[0])}](
              ibmw::extension::MessageView& msg_wrapper,
              std::function<void()>&& release_callback) {
            auto cur_timestamp = ibmw::common::util::GetCurTimestampNs();

            record_func(cur_timestamp, msg_wrapper);

            release_callback();
          };
    } else {
      callback =
          [record_func_vec{std::move(record_func_vec)}](
              ibmw::extension::MessageView& msg_wrapper,
              std::function<void()>&& release_callback) {
            auto cur_timestamp = ibmw::common::util::GetCurTimestampNs();

            for (const auto& record_func : record_func_vec)
              record_func(cur_timestamp, msg_wrapper);

            release_callback();
          };
    }

    bool ret = runtime_->Subscribe(std::move(subscription), std::move(callback));
    MW_REQUIRE(ret, "Subscribe failed!");
  }
}

void RecordPlaybackExtension::RegisterPlaybackTransport() {
  std::unordered_set<ibmw::transport::TopicMetaKey,
                     ibmw::transport::TopicMetaKey::Hash>
      playback_topic_meta_set;

  // Handle playback action
  for (auto& playback_action_itr : playback_action_map_) {
    auto& playback_action = *(playback_action_itr.second);

    const auto& topic_meta_map = playback_action.GetTopicMetaMap();

    for (const auto& topic_meta_itr : topic_meta_map) {
      playback_topic_meta_set.emplace(ibmw::transport::TopicMetaKey{
          .topic_name = topic_meta_itr.second.topic_name,
          .msg_type = topic_meta_itr.second.msg_type});
    }
  }

  // RegisterSendType
  for (const auto& item : playback_topic_meta_set) {
    const auto* type_support_wrapper = FindTypeSupport(item.msg_type);
    MW_REQUIRE(
        type_support_wrapper != nullptr,
        "Cannot find TypeSupport for playback type '{}'.", item.msg_type);
    auto type_support_ref = type_support_wrapper->type_support_ref;

    const auto publisher = runtime_->RegisterPublisher(
        {.topic = item.topic_name,
         .msg_type = item.msg_type,
         .package_path = type_support_wrapper->package_path,
         .module_name = "core",
         .type_support = type_support_ref});
    MW_REQUIRE(publisher, "Register publish type failed!");
  }

  // RegisterPubRecordFunc
  for (auto& playback_action_itr : playback_action_map_) {
    struct TopicIndexWrapper {
      ibmw::extension::PublisherHandle publisher;
      std::string serialization_type;
    };
    std::unordered_map<uint64_t, TopicIndexWrapper> topic_id_wrapper_map;

    auto& playback_action = *(playback_action_itr.second);
    const auto& topic_meta_map = playback_action.GetTopicMetaMap();

    for (const auto& topic_meta_itr : topic_meta_map) {
      const auto& topic_meta = topic_meta_itr.second;
      const auto* type_support_wrapper = FindTypeSupport(topic_meta.msg_type);
      MW_REQUIRE(
          type_support_wrapper != nullptr,
          "Cannot find TypeSupport for playback type '{}'.",
          topic_meta.msg_type);

      const auto publisher = runtime_->RegisterPublisher(
          {.topic = topic_meta.topic_name,
           .msg_type = topic_meta.msg_type,
           .package_path = type_support_wrapper->package_path,
           .module_name = "core",
           .type_support = type_support_wrapper->type_support_ref});
      MW_REQUIRE(publisher, "Get publish type failed!");

      topic_id_wrapper_map.emplace(
          topic_meta.id,
          TopicIndexWrapper{
              .publisher = publisher,
              .serialization_type = topic_meta.serialization_type});
    }

    playback_action.RegisterPubRecordFunc(
        [this, topic_id_wrapper_map{std::move(topic_id_wrapper_map)}](
            const PlaybackAction::OneRecord& record) {
          auto finditr = topic_id_wrapper_map.find(record.topic_index);
          if (finditr == topic_id_wrapper_map.end()) [[unlikely]] {
            MW_WARN("Invalid topic id: {}.", record.topic_index);
            return;
          }

          const auto& serialization_type = finditr->second.serialization_type;

          ibmw::transport::Context ctx;
          runtime_->Send(finditr->second.publisher, ctx, serialization_type,
                         record.buffer_view_ptr);
        });
  }
}

void RecordPlaybackExtension::RegisterRuntimePlaybackTransport() {
  for (const auto& subscriber : runtime_->GetTransportEndpoints()) {
    if (subscriber.published) {
      continue;
    }
    {
      const auto& topic_name = subscriber.topic;
      const std::string key = PlaybackPublisherKey(
          topic_name, subscriber.msg_type);
      if (runtime_playback_publishers_.contains(key)) {
        continue;
      }

      const auto publisher = runtime_->RegisterPublisher(
          {.topic = subscriber.topic,
           .msg_type = subscriber.msg_type,
           .package_path = subscriber.package_path,
           .module_name = std::string(kRuntimePlaybackModule),
           .type_support = subscriber.type_support});
      MW_REQUIRE(
          publisher,
          "Register Runtime playback publisher failed for topic '{}'.",
          topic_name);
      runtime_playback_publishers_.emplace(key, publisher);
    }
  }
}

void RecordPlaybackExtension::PublishRuntimePlaybackMessage(
    const TopicAdvertisement& advertisement,
    ibmw::transport::SerializedMessage&& message) {
  const auto publisher = runtime_playback_publishers_.find(
      PlaybackPublisherKey(
          advertisement.topic.topic, advertisement.topic.msg_type));
  MW_REQUIRE(
      publisher != runtime_playback_publishers_.end(),
      "No Runtime playback publisher is registered for topic '{}'.",
      advertisement.topic.topic);
  MW_REQUIRE(
      message.payload != nullptr &&
          message.topic.serialization_type ==
              advertisement.topic.serialization_type,
      "Playback serialization does not match topic '{}'.",
      advertisement.topic.topic);

  ibmw::transport::Context context;
  context.SetMetaValue("ibmw.playback_session", message.identity.source_id);
  runtime_->Send(publisher->second, context, message.topic.serialization_type,
                 std::move(message.payload));
}

}  // namespace ibmw::extensions::record_playback_extension

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_v2_t IbmwExtEntryV2() {
  using Extension =
      ibmw::extensions::record_playback_extension::RecordPlaybackExtension;
  return ibmw_ext_entry_v2_t{
      .abi_version = kIbmwExtensionAbiVersionV2,
      .struct_size = sizeof(ibmw_ext_entry_v2_t),
      .create = []() -> ibmw::EngineExtensionBase* { return new Extension(); },
      .destroy = [](const ibmw::EngineExtensionBase* extension) {
        delete extension;
      }};
}
