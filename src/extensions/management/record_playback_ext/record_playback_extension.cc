// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/record_playback_extension.h"

#include <set>
#include <string>
#include <utility>

#include "cpp/service/rpc_handle.h"
#include "cpp/executor/executor.h"
#include "cpp/util/type_support.h"
#include "extension_api/extension_logger.h"
#include "extension_api/ibmw_extension_main.h"
#include "extension_api/ibmw_extension_runtime.h"
#include "extension_api/ibmw_runtime_graph.h"
#include "record_playback_ext/global.h"
#include "base/time_util.h"
#include "record_playback_ext/playback_action.h"
#include "record_playback_ext/record_action.h"
#include "record_playback_ext/service.h"
#include "record_playback_ext/runtime/runtime_playback_stream.h"
#include "record_playback_ext/runtime/runtime_tap_stream.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::TopicAdvertisement;

class RecordPlaybackExtension : public ibmw::EngineExtensionBase {
 public:
  struct Options {
    std::string service_name;
    std::string timer_executor;
    ibmw::extension::RuntimeGraphOptions graph_options;
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
  void RegisterRpcService();
  void RegisterRecordTransport();
  void RegisterPlaybackTransport();
  void RegisterRuntimePlaybackTransport();
  void PublishRuntimePlaybackMessage(
      const TopicAdvertisement& advertisement,
      ibmw::transport::SerializedMessage&& message);
  void StartRuntimeAgent();
  void StopRuntimeAgent();

  Options options_;
  ibmw::extension::RuntimeGraphV1* graph_ = nullptr;
  ibmw::extension::RuntimeV1* runtime_ = nullptr;
  bool initialized_ = false;
  std::unique_ptr<RecordPlaybackServiceImpl> service_ptr_;
  ibmw::executor::ExecutorHandle timer_executor_ref_;

  ibmw::util::TypeSupportRef FindTypeSupport(
      std::string_view msg_type, std::string_view package_path = {}) const;
  std::string TypeSupportPackage(
      std::string_view msg_type, std::string_view package_path = {}) const;
  std::unique_ptr<RuntimeTapServer> runtime_tap_server_;
  std::unique_ptr<PublishTapCapability> publish_tap_capability_;
  std::unique_ptr<RuntimePlaybackServer> runtime_playback_server_;
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

    const auto& graph = rhs.graph_options;
    if (graph.enabled) node["enable_runtime_agent"] = *graph.enabled;
    if (graph.runtime_id) node["runtime_id"] = *graph.runtime_id;
    if (graph.registry_path) {
      node["runtime_registry_path"] = graph.registry_path->string();
    }
    for (const auto& path : graph.type_support_pkgs) {
      Node package;
      package["path"] = path;
      node["type_support_pkgs"].push_back(package);
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

    if (node["timer_executor"])
      rhs.timer_executor = node["timer_executor"].as<std::string>();

    auto& graph = rhs.graph_options;
    if (node["enable_runtime_agent"]) {
      graph.enabled = node["enable_runtime_agent"].as<bool>();
    }
    if (node["runtime_id"]) graph.runtime_id = node["runtime_id"].as<std::string>();
    if (node["runtime_registry_path"]) {
      graph.registry_path = node["runtime_registry_path"].as<std::string>();
    }
    if (node["type_support_pkgs"] && node["type_support_pkgs"].IsSequence()) {
      for (const auto& package : node["type_support_pkgs"]) {
        graph.type_support_pkgs.push_back(package["path"].as<std::string>());
      }
    }

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
    auto* graph_access = dynamic_cast<ibmw::extension::RuntimeGraphAccessV1*>(runtime_);
    MW_REQUIRE(graph_access, "Record/playback requires Runtime Graph capability V1.");
    graph_ = &graph_access->GetRuntimeGraph();
    YAML::Node extension_options_node = runtime_->GetExtensionOptions(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }
  graph_->ApplyFallbackOptions(options_.graph_options);
  auto& runtime_graph = *graph_;
  if (runtime_graph.Enabled()) {
    EnsureRuntimePlaybackPublisherRules(*runtime_);
  }

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
          return FindTypeSupport(msg_type);
        });
    action_ptr->RegisterGetSchemaSupportFunc(
        [this](std::string_view msg_type, std::string_view serialization_type) {
          return graph_->FindSchema(
              msg_type, serialization_type);
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
          return FindTypeSupport(msg_type);
        });
    action_ptr->Initialize(playback_action_options.options);

    playback_action_map_.emplace(playback_action_options.name, std::move(action_ptr));
  }

  runtime_graph.HideModulePublishers(std::string(kRuntimePlaybackModule));
  runtime_graph.SetStartTapHandler(
      [this](const TopicAdvertisement& advertisement) {
        MW_REQUIRE(runtime_tap_server_, "Runtime tap server is unavailable.");
        return runtime_tap_server_->StartSession(advertisement);
      });
  runtime_graph.SetStopTapHandler(
      [this](std::string_view session_id) {
        return runtime_tap_server_ && runtime_tap_server_->StopSession(session_id);
      });
  runtime_graph.SetStartPlaybackHandler(
      [this](const TopicAdvertisement& advertisement) {
        MW_REQUIRE(runtime_playback_server_, "Runtime playback server is unavailable.");
        return runtime_playback_server_->StartSession(advertisement);
      });
  runtime_graph.SetStopPlaybackHandler(
      [this](std::string_view session_id) {
        return runtime_playback_server_ && runtime_playback_server_->StopSession(session_id);
      });

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
      [this] {
        RegisterRuntimePlaybackTransport();
      });

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kInitialize,
       ibmw::extension::RuntimeSubsystem::kRuntimeGraph,
       ibmw::extension::RuntimeHookTiming::kAfter},
      [this] { StartRuntimeAgent(); });

  runtime_->RegisterHook(
      {ibmw::extension::RuntimePhase::kStopping,
       ibmw::extension::RuntimeSubsystem::kRuntimeGraph,
       ibmw::extension::RuntimeHookTiming::kAfter},
      [this] { StopRuntimeAgent(); });

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

void RecordPlaybackExtension::StartRuntimeAgent() {
  auto& runtime_graph = *graph_;
  if (!runtime_graph.Enabled() || runtime_tap_server_) {
    return;
  }
  const auto& registration = runtime_graph.Registration();
  const auto& runtime_id = registration.runtime_id;
  publish_tap_capability_ =
      std::make_unique<RuntimePublishTapCapability>(*runtime_);
  runtime_tap_server_ = std::make_unique<RuntimeTapServer>(
      *publish_tap_capability_, runtime_id,
      runtime_graph.RegistryPath());
  runtime_tap_server_->Start();
  runtime_playback_server_ = std::make_unique<RuntimePlaybackServer>(
      runtime_id, runtime_graph.RegistryPath(),
      [this](const TopicAdvertisement& advertisement, auto&& message) {
        PublishRuntimePlaybackMessage(
            advertisement, std::move(message));
      });
  runtime_playback_server_->Start();

}

void RecordPlaybackExtension::StopRuntimeAgent() {
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

ibmw::util::TypeSupportRef RecordPlaybackExtension::FindTypeSupport(
    std::string_view msg_type,
    std::string_view package_path) const {
  return graph_->FindTypeSupport(
      msg_type, package_path);
}

std::string RecordPlaybackExtension::TypeSupportPackage(
    std::string_view msg_type,
    std::string_view package_path) const {
  return graph_->TypeSupportPackage(
      msg_type, package_path);
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

    const auto type_support_ref = FindTypeSupport(key.msg_type);
    MW_REQUIRE(
        type_support_ref,
        "Cannot find TypeSupport for record type '{}'.", key.msg_type);
    const auto package_path = TypeSupportPackage(key.msg_type);

    // Small optimization
    auto& record_func_vec = wrapper.record_func_vec;
    ibmw::extension::SubscriptionOptions subscription{
        .topic = key.topic_name,
        .msg_type = key.msg_type,
        .package_path = package_path,
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
    const auto type_support_ref = FindTypeSupport(item.msg_type);
    MW_REQUIRE(
        type_support_ref,
        "Cannot find TypeSupport for playback type '{}'.", item.msg_type);
    const auto package_path = TypeSupportPackage(item.msg_type);

    const auto publisher = runtime_->RegisterPublisher(
        {.topic = item.topic_name,
         .msg_type = item.msg_type,
         .package_path = package_path,
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
      const auto type_support_ref = FindTypeSupport(topic_meta.msg_type);
      MW_REQUIRE(
          type_support_ref,
          "Cannot find TypeSupport for playback type '{}'.",
          topic_meta.msg_type);
      const auto package_path = TypeSupportPackage(topic_meta.msg_type);

      const auto publisher = runtime_->RegisterPublisher(
          {.topic = topic_meta.topic_name,
           .msg_type = topic_meta.msg_type,
           .package_path = package_path,
           .module_name = "core",
           .type_support = type_support_ref});
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
