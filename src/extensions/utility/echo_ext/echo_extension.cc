// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "echo_ext/echo_extension.h"

#include <string>

#include "engine/engine.h"
#include "engine/transport/transport_driver_tools.h"
#include "echo_ext/global.h"

#include "json/json.h"
#include "yaml-cpp/yaml.h"

namespace YAML {

template <>
struct convert<ibmw::extensions::echo_extension::EchoExtension::Options> {
  using Options = ibmw::extensions::echo_extension::EchoExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["type_support_pkgs"] = Node(NodeType::Sequence);
    for (const auto& type_support_pkg : rhs.type_support_pkgs) {
      Node type_support_pkg_node;
      type_support_pkg_node["path"] = type_support_pkg.path;
      node["type_support_pkgs"].push_back(type_support_pkg_node);
    }

    node["topic_meta_list"] = Node(NodeType::Sequence);

    for (const auto& topic_meta : rhs.topic_meta_list) {
      Node topic_meta_node;
      topic_meta_node["topic"] = topic_meta.topic_name;
      topic_meta_node["msg_type"] = topic_meta.msg_type;
      topic_meta_node["echo_type"] = topic_meta.echo_type;
      node["topic_meta_list"].push_back(topic_meta_node);
    }
    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["type_support_pkgs"] && node["type_support_pkgs"].IsSequence()) {
      for (const auto& type_support_pkg_node : node["type_support_pkgs"]) {
        Options::TypeSupportPkg type_support_pkg;
        type_support_pkg.path = type_support_pkg_node["path"].as<std::string>();
        rhs.type_support_pkgs.emplace_back(std::move(type_support_pkg));
      }
    }

    if (node["topic_meta_list"] && node["topic_meta_list"].IsSequence()) {
      for (const auto& topic_meta_node : node["topic_meta_list"]) {
        Options::TopicMeta topic_meta;
        topic_meta.topic_name = topic_meta_node["topic"].as<std::string>();
        topic_meta.msg_type = topic_meta_node["msg_type"].as<std::string>();
        if (topic_meta_node["echo_type"] && topic_meta_node["echo_type"].IsScalar()) {
          topic_meta.echo_type = topic_meta_node["echo_type"].as<std::string>();
        } else {
          topic_meta.echo_type = "json";
        }
        rhs.topic_meta_list.push_back(std::move(topic_meta));
      }
    }
    return true;
  }
};

}  // namespace YAML

namespace ibmw::extensions::echo_extension {

EchoExtension::EchoExtension() {
  SetExtensionLogger(&g_logger);
}

bool EchoExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
    MW_TRACE("Load extension options.");
  }

  // type support
  for (auto& type_support_pkg : options_.type_support_pkgs) {
    // check duplicate pkg
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

  for (auto& topic_meta : options_.topic_meta_list) {
    // check msg type
    auto finditr = type_support_map_.find(topic_meta.msg_type);
    MW_REQUIRE(finditr != type_support_map_.end(),
                            "Can not find type '{}' in any type support pkg!", topic_meta.msg_type);
  }

  // check duplicate topic
  for (auto& topic_meta_option : options_.topic_meta_list) {
    runtime::core::util::TopicMetaKey key{
        .topic_name = topic_meta_option.topic_name,
        .msg_type = topic_meta_option.msg_type};

    MW_REQUIRE(
        topic_meta_map_.find(key) == topic_meta_map_.end(),
        "Duplicate topic meta, topic name: {}, msg type: {}.",
        topic_meta_option.topic_name, topic_meta_option.msg_type);

    TopicMeta topic_meta{
        .topic_name = topic_meta_option.topic_name,
        .msg_type = topic_meta_option.msg_type,
        .echo_type = topic_meta_option.echo_type,
    };
    topic_meta_map_.emplace(key, topic_meta);
  }

  core_ptr_->RegisterHook(
      runtime::core::Before(runtime::core::Phase::kInitialize,
                            runtime::core::Subsystem::kModule),
      [this] {
        RegisterEchoTransport();
      });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void EchoExtension::InitTypeSupport(Options::TypeSupportPkg& options) {
  auto loader_ptr = std::make_unique<ibmw::runtime::core::util::TypeSupportPkgLoader>();
  loader_ptr->LoadTypeSupportPkg(options.path);

  options.path = loader_ptr->GetDynamicLib().GetLibFullPath();

  auto type_support_array = loader_ptr->GetTypeSupportArray();
  for (const auto* item : type_support_array) {
    ibmw::util::TypeSupportRef type_support_ref(item);
    auto type_name = type_support_ref.TypeName();

    // check duplicate type
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
  type_support_pkg_loader_vec_.emplace_back(std::move(loader_ptr));
}

void EchoExtension::RegisterEchoTransport() {
  using namespace ibmw::runtime::core::transport;

  const auto& topic_meta_list = topic_meta_map_;
  MW_TRACE("Echo extension has {} topics.", topic_meta_list.size());

  for (const auto& topic_meta_itr : topic_meta_list) {
    const auto& topic_meta = topic_meta_itr.second;

    auto finditr = type_support_map_.find(topic_meta.msg_type);
    MW_REQUIRE(finditr != type_support_map_.end(),
                            "Can not find type '{}' in any type support pkg!", topic_meta.msg_type);

    const auto& type_support_wrapper = finditr->second;

    MessageHandlerWrapper msg_handler;
    msg_handler.info = TransportInfo{
        .msg_type = topic_meta.msg_type,
        .topic_name = topic_meta.topic_name,
        .pkg_path = type_support_wrapper.options.path,
        .module_name = "core",
        .msg_type_support_ref = type_support_wrapper.type_support_ref};

    msg_handler.require_cache_serialization_types.emplace(topic_meta.echo_type);
    msg_handler.callback = [echo_type{topic_meta.echo_type}, this](
                               MessageFrame& msg_wrapper, std::function<void()>&& release_callback,
                               const internal::CallbackInvocationEnvelopeV1*) {
      auto buffer_view_ptr = ibmw::runtime::core::transport::TrySerializeMsgWithCache(msg_wrapper, echo_type);
      if (!buffer_view_ptr) [[unlikely]] {
        MW_ERROR("Can not serialize msg type '{}' with echo type '{}'.",
                    msg_wrapper.info.msg_type, echo_type);
        release_callback();
        return;
      }
      std::string echo_str = buffer_view_ptr->JoinToString();
      if (echo_type == "json") {
        FormatJson(echo_str);
      }
      if (!echo_str.empty()) [[unlikely]]
        MW_INFO("\n---\n{}\n---\n", echo_str);
      release_callback();
    };

    bool ret = core_ptr_->GetTransportBus().OnMessage(std::move(msg_handler));
    MW_REQUIRE(ret, "Subscribe failed!");
  }
}

void EchoExtension::FormatJson(std::string& json_str) {
  Json::Value root;
  bool success = Json::Reader().parse(json_str, root);
  if (success) {
    json_str = Json::StyledWriter().write(root);
  } else [[unlikely]] {
    MW_WARN("Invalid json string, will keep original string.");
  }
}

void EchoExtension::OnShutdown() {
  // No cleanup needed beyond what ExtensionBase handles.
}

}  // namespace ibmw::extensions::echo_extension

IBMW_EXT_ENTRY(ibmw::extensions::echo_extension::EchoExtension)
