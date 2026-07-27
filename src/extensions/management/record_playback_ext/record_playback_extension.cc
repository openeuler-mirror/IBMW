// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/record_playback_extension.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

#include "cpp/service/rpc_handle.h"
#include "engine/engine.h"
#include "engine/transport/transport_driver_tools.h"
#include "record_playback_ext/global.h"
#include "base/time_util.h"
#include "engine/core_types.h"

namespace YAML {

template <>
struct convert<ibmw::extensions::record_playback_extension::RecordPlaybackExtension::Options> {
  using Options = ibmw::extensions::record_playback_extension::RecordPlaybackExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["service_name"] = rhs.service_name;
    node["timer_executor"] = rhs.timer_executor;

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

RecordPlaybackExtension::RecordPlaybackExtension() {
  SetExtensionLogger(&g_logger);
}

bool RecordPlaybackExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
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
          return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
        });
    action_ptr->RegisterGetTypeSupportFunc(
        [this](std::string_view msg_type) -> ibmw::util::TypeSupportRef {
          auto finditr = type_support_map_.find(msg_type);
          if (finditr != type_support_map_.end())
            return finditr->second.type_support_ref;
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
          return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
        });
    action_ptr->RegisterGetTypeSupportFunc(
        [this](std::string_view msg_type) -> ibmw::util::TypeSupportRef {
          auto finditr = type_support_map_.find(msg_type);
          if (finditr != type_support_map_.end())
            return finditr->second.type_support_ref;
          return {};
        });
    action_ptr->Initialize(playback_action_options.options);

    playback_action_map_.emplace(playback_action_options.name, std::move(action_ptr));
  }

  core_ptr_->RegisterHook(
      runtime::core::Before(runtime::core::Phase::kInitialize,
                            runtime::core::Subsystem::kModule),
      [this] {
        RegisterRpcService();
        RegisterRecordTransport();
        RegisterPlaybackTransport();
      });

  core_ptr_->RegisterHook(
      runtime::core::After(runtime::core::Phase::kInitialize,
                           runtime::core::Subsystem::kExecutor),
      [this] {
        if (record_action_map_.size() != 0) {
          timer_executor_ref_ = core_ptr_->GetExecutorBus().GetExecutor(options_.timer_executor);
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

  core_ptr_->RegisterHook(
      runtime::core::AfterPhase(runtime::core::Phase::kStart),
      [this] {
        for (auto& itr : record_action_map_) {
          itr.second->Start();
        }
        for (auto& itr : playback_action_map_) {
          itr.second->Start();
        }
      });

  core_ptr_->RegisterHook(
      runtime::core::BeforePhase(runtime::core::Phase::kStopping),
      [this] {
        for (auto& itr : playback_action_map_)
          itr.second->Shutdown();

        for (auto& itr : record_action_map_)
          itr.second->Shutdown();
      });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void RecordPlaybackExtension::OnShutdown() {
  for (auto& itr : playback_action_map_)
    itr.second->Shutdown();

  for (auto& itr : record_action_map_)
    itr.second->Shutdown();
}

void RecordPlaybackExtension::InitTypeSupport(Options::TypeSupportPkg& options) {
  auto loader_ptr = std::make_unique<runtime::core::util::TypeSupportPkgLoader>();
  loader_ptr->LoadTypeSupportPkg(options.path);

  options.path = loader_ptr->GetDynamicLib().GetLibFullPath();

  auto type_support_array = loader_ptr->GetTypeSupportArray();
  auto schema_support_array = loader_ptr->GetSchemaSupportArray();

  for (const auto* item : type_support_array) {
    ibmw::util::TypeSupportRef type_support_ref(item);
    auto type_name = type_support_ref.TypeName();

    // Check for duplicate type
    auto finditr = type_support_map_.find(type_name);
    if (finditr != type_support_map_.end()) {
      MW_WARN("Duplicate msg type '{}' in {} and {}.",
                 type_name, options.path, finditr->second.options.path);
      continue;
    }

    type_support_map_.emplace(
        type_name,
        TypeSupportWrapper{
            .options = options,
            .type_support_ref = type_support_ref,
            .loader_ptr = loader_ptr.get()});
  }

  for (const auto* item : schema_support_array) {
    schema_registry_.Register(ibmw::util::SchemaSupportRef(item));
  }

  type_support_pkg_loader_vec_.emplace_back(std::move(loader_ptr));
}

void RecordPlaybackExtension::RegisterRpcService() {
  service_ptr_ = std::make_unique<RecordPlaybackServiceImpl>();

  if (!options_.service_name.empty())
    service_ptr_->SetServiceName(options_.service_name);

  service_ptr_->SetRecordActionMap(&record_action_map_);
  service_ptr_->SetPlaybackActionMap(&playback_action_map_);

  auto service_ref = ibmw::rpc::ServiceRef(
      core_ptr_->GetServiceDispatcher().GetRpcAccess().NativeHandle());

  bool ret = service_ref.RegisterService(service_ptr_.get());
  MW_CHECK_ERROR(ret, "Register service failed.");
}

void RecordPlaybackExtension::RegisterRecordTransport() {
  using namespace ibmw::runtime::core::transport;
  using RecordFunc = std::function<void(uint64_t, MessageFrame&)>;

  struct Wrapper {
    std::unordered_set<std::string> require_cache_serialization_types;
    std::vector<RecordFunc> record_func_vec;
  };

  std::unordered_map<ibmw::runtime::core::util::TopicMetaKey, Wrapper,
                     ibmw::runtime::core::util::TopicMetaKey::Hash>
      recore_func_map;

  for (auto& record_action_itr : record_action_map_) {
    auto& record_action = *(record_action_itr.second);

    const auto& topic_meta_map = record_action.GetTopicMetaMap();

    // RecordFunc
    for (const auto& topic_meta_itr : topic_meta_map) {
      const auto& topic_meta = topic_meta_itr.second;

      RecordFunc record_func =
          [&record_action, topic_id{topic_meta.id}, serialization_type{topic_meta.serialization_type}](
              uint64_t cur_timestamp, MessageFrame& msg_wrapper) {
            auto buffer_view_ptr = ibmw::runtime::core::transport::TrySerializeMsgWithCache(msg_wrapper, serialization_type);
            if (!buffer_view_ptr) [[unlikely]] {
              MW_WARN("Can not serialize msg type '{}' with serialization type '{}'.",
                         msg_wrapper.info.msg_type, serialization_type);
              return;
            }
            auto pub_ts = std::string(msg_wrapper.ctx_ref.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP));
            auto pub_seq = std::string(msg_wrapper.ctx_ref.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ));

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

    auto finditr = type_support_map_.find(key.msg_type);

    const auto& type_support_wrapper = finditr->second;
    auto type_support_ref = type_support_wrapper.type_support_ref;

    MessageHandlerWrapper msg_handler;
    msg_handler.info = TransportInfo{
        .msg_type = key.msg_type,
        .topic_name = key.topic_name,
        .pkg_path = type_support_wrapper.options.path,
        .module_name = "core",
        .msg_type_support_ref = type_support_ref};

    msg_handler.require_cache_serialization_types = wrapper.require_cache_serialization_types;

    // Small optimization
    auto& record_func_vec = wrapper.record_func_vec;
    if (record_func_vec.size() == 1) {
      msg_handler.callback =
          [record_func{std::move(record_func_vec[0])}](
              MessageFrame& msg_wrapper, std::function<void()>&& release_callback) {
            auto cur_timestamp = ibmw::common::util::GetCurTimestampNs();

            record_func(cur_timestamp, msg_wrapper);

            release_callback();
          };
    } else {
      msg_handler.callback =
          [record_func_vec{std::move(record_func_vec)}](
              MessageFrame& msg_wrapper, std::function<void()>&& release_callback) {
            auto cur_timestamp = ibmw::common::util::GetCurTimestampNs();

            for (const auto& record_func : record_func_vec)
              record_func(cur_timestamp, msg_wrapper);

            release_callback();
          };
    }

    bool ret = core_ptr_->GetTransportBus().OnMessage(std::move(msg_handler));
    MW_REQUIRE(ret, "Subscribe failed!");
  }
}

void RecordPlaybackExtension::RegisterPlaybackTransport() {
  using namespace ibmw::runtime::core::transport;

  std::unordered_set<ibmw::runtime::core::util::TopicMetaKey,
                     ibmw::runtime::core::util::TopicMetaKey::Hash>
      playback_topic_meta_set;

  // Handle playback action
  for (auto& playback_action_itr : playback_action_map_) {
    auto& playback_action = *(playback_action_itr.second);

    const auto& topic_meta_map = playback_action.GetTopicMetaMap();

    for (const auto& topic_meta_itr : topic_meta_map) {
      playback_topic_meta_set.emplace(ibmw::runtime::core::util::TopicMetaKey{
          .topic_name = topic_meta_itr.second.topic_name,
          .msg_type = topic_meta_itr.second.msg_type});
    }
  }

  // RegisterSendType
  for (const auto& item : playback_topic_meta_set) {
    auto finditr = type_support_map_.find(item.msg_type);

    const auto& type_support_wrapper = finditr->second;
    auto type_support_ref = type_support_wrapper.type_support_ref;

    SendTypeWrapper send_type_wrapper;
    send_type_wrapper.info = TransportInfo{
        .msg_type = item.msg_type,
        .topic_name = item.topic_name,
        .pkg_path = type_support_wrapper.options.path,
        .module_name = "core",
        .msg_type_support_ref = type_support_ref};

    bool ret = core_ptr_->GetTransportBus().RegisterSendType(std::move(send_type_wrapper));
    MW_REQUIRE(ret, "Register publish type failed!");
  }

  // RegisterPubRecordFunc
  for (auto& playback_action_itr : playback_action_map_) {
    struct TopicIndexWrapper {
      const ibmw::runtime::core::transport::SendTypeWrapper* send_type_wrapper_ptr;
      std::string serialization_type;
    };
    std::unordered_map<uint64_t, TopicIndexWrapper> topic_id_wrapper_map;

    auto& playback_action = *(playback_action_itr.second);
    const auto& topic_meta_map = playback_action.GetTopicMetaMap();

    for (const auto& topic_meta_itr : topic_meta_map) {
      const auto& topic_meta = topic_meta_itr.second;
      auto finditr = type_support_map_.find(topic_meta.msg_type);
      const auto& type_support_wrapper = finditr->second;

      const auto* send_type_wrapper_ptr = core_ptr_->GetTransportBus().GetTransportCatalog()->GetSendTypeWrapperPtr(
          topic_meta.msg_type, topic_meta.topic_name, type_support_wrapper.options.path, "core");
      MW_REQUIRE(send_type_wrapper_ptr, "Get publish type failed!");

      topic_id_wrapper_map.emplace(
          topic_meta.id,
          TopicIndexWrapper{
              .send_type_wrapper_ptr = send_type_wrapper_ptr,
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

          const auto* send_type_wrapper_ptr = finditr->second.send_type_wrapper_ptr;
          const auto& serialization_type = finditr->second.serialization_type;

          ibmw::transport::Context ctx;

          MessageFrame msg_wrapper{
              .info = send_type_wrapper_ptr->info,
              .msg_ptr = nullptr,
              .ctx_ref = ctx};

          msg_wrapper.serialization_cache.emplace(serialization_type, record.buffer_view_ptr);

          core_ptr_->GetTransportBus().Send(std::move(msg_wrapper));
        });
  }
}

}  // namespace ibmw::extensions::record_playback_extension

IBMW_EXT_ENTRY(ibmw::extensions::record_playback_extension::RecordPlaybackExtension)
