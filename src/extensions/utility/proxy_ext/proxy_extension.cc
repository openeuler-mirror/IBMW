// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "proxy_ext/proxy_extension.h"

#include <string>

#include "engine/transport/transport_driver_tools.h"
#include "engine/transport/message_frame.h"
#include "engine/transport/transport_catalog.h"
#include "proxy_ext/global.h"

namespace YAML {

template <>
struct convert<ibmw::extensions::proxy_extension::ProxyExtension::Options> {
  using Options = ibmw::extensions::proxy_extension::ProxyExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["type_support_pkgs"] = Node(NodeType::Sequence);
    for (const auto& type_support_pkg : rhs.type_support_pkgs) {
      Node type_support_pkg_node;
      type_support_pkg_node["path"] = type_support_pkg.path;
      node["type_support_pkgs"].push_back(type_support_pkg_node);
    }

    node["executor"] = rhs.executor;
    node["proxy_actions"] = Node(NodeType::Sequence);

    for (const auto& proxy_action : rhs.proxy_actions) {
      Node proxy_action_node;
      proxy_action_node["name"] = proxy_action.name;
      proxy_action_node["options"] = proxy_action.options;
      node["proxy_actions"].push_back(proxy_action_node);
    }
    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["type_support_pkgs"] && node["type_support_pkgs"].IsSequence()) {
      for (const auto& type_support_pkg_node : node["type_support_pkgs"]) {
        Options::TypeSupportPkg type_support_pkg;
        type_support_pkg.path = type_support_pkg_node["path"].as<std::string>();
        rhs.type_support_pkgs.push_back(std::move(type_support_pkg));
      }
    }

    if (node["executor"] && node["executor"].IsScalar()) {
      rhs.executor = node["executor"].as<std::string>();
    }

    if (node["proxy_actions"] && node["proxy_actions"].IsSequence()) {
      for (const auto& proxy_action_node : node["proxy_actions"]) {
        Options::ProxyAction proxy_action;
        proxy_action.name = proxy_action_node["name"].as<std::string>();
        proxy_action.options = proxy_action_node["options"];
        rhs.proxy_actions.push_back(std::move(proxy_action));
      }
    }
    return true;
  }
};

}  // namespace YAML

namespace ibmw::extensions::proxy_extension {

ProxyExtension::ProxyExtension() {
  SetExtensionLogger(&g_logger);
}

bool ProxyExtension::OnInit(runtime::core::Engine* core_ptr) {
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

  // proxy action
  for (auto& proxy_action : options_.proxy_actions) {
    // check duplicate proxy action name
    auto finditr = std::find_if(
        options_.proxy_actions.begin(), options_.proxy_actions.end(),
        [&proxy_action](const auto& op) {
          if (&proxy_action == &op) return false;
          return op.name == proxy_action.name;
        });
    MW_REQUIRE(finditr == options_.proxy_actions.end(),
                            "Duplicate proxy action name {}", proxy_action.name);

    auto action_ptr = std::make_unique<ProxyAction>();

    action_ptr->RegisterGetExecutorFunc([this](std::string_view name) -> ibmw::executor::ExecutorHandle {
      return core_ptr_->GetExecutorBus().GetExecutor(name);
    });
    action_ptr->RegisterGetTypeSupportFunc([this](std::string_view name) -> ibmw::util::TypeSupportRef {
      auto finditr = type_support_map_.find(name);
      MW_REQUIRE(finditr != type_support_map_.end(),
                              "Can not find type support for msg type '{}'.", name);
      return finditr->second.type_support_ref;
    });
    action_ptr->Initialize(proxy_action.options);

    proxy_action_map_.emplace(proxy_action.name, std::move(action_ptr));
  }

  core_ptr_->RegisterHook(
      runtime::core::Before(runtime::core::Phase::kInitialize,
                            runtime::core::Subsystem::kModule),
      [this] {
        for (auto& proxy_action_itr : proxy_action_map_) {
          proxy_action_itr.second->InitExecutor();
        }
        RegisterSubTransport();
        RegisterPubTransport();
      });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);
  return true;
}

void ProxyExtension::InitTypeSupport(Options::TypeSupportPkg& options) {
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
  MW_TRACE("Load {} type support pkgs.", type_support_pkg_loader_vec_.size());
}

void ProxyExtension::RegisterSubTransport() {
  using namespace ibmw::runtime::core::transport;

  for (const auto& [_, proxy_action] : proxy_action_map_) {
    const auto& action_topic_meta_map = proxy_action->GetTopicMetaMap();
    for (const auto& [_, topic_meta] : action_topic_meta_map) {
      auto finditr = type_support_map_.find(topic_meta.msg_type);
      MW_REQUIRE(finditr != type_support_map_.end(),
                              "Can not find type '{}' in any type support pkg!", topic_meta.msg_type);

      const auto& type_support_wrapper = finditr->second;

      MessageHandlerWrapper message_handler{
          .info = {
              .msg_type = topic_meta.msg_type,
              .topic_name = topic_meta.topic_name,
              .pkg_path = type_support_wrapper.options.path,
              .module_name = "core",
              .msg_type_support_ref = type_support_wrapper.type_support_ref}};

      for (const auto type_list = type_support_wrapper.type_support_ref.SerializationTypesSupportedListSpan(); const auto& type : type_list) {
        message_handler.require_cache_serialization_types.emplace(type.str);
      }

      message_handler.callback = [this, action_raw_ptr = proxy_action.get()](
                                       MessageFrame& msg_wrapper, std::function<void()>&& release_callback,
                                       const internal::CallbackInvocationEnvelopeV1*) {
        if (msg_wrapper.msg_ptr == nullptr && msg_wrapper.serialization_cache.size() == 0) [[unlikely]] {
          MW_WARN("Receive empty msg, ignore it.");
          release_callback();
          return;
        }
        action_raw_ptr->GetExecutor().Dispatch([this, msg_wrapper, topic_meta_map = action_raw_ptr->GetTopicMetaMap()]() {

          runtime::core::util::TopicMetaKey key{
            .topic_name = msg_wrapper.info.topic_name,
            .msg_type = msg_wrapper.info.msg_type,
          };

          auto finditr = topic_meta_map.find(key);
          MW_REQUIRE(finditr != topic_meta_map.end(),
                                  "Can not find topic meta, topic name: {}, msg type: {}.",
                                  key.topic_name, key.msg_type);

          for (auto &pub_topic_name : finditr->second.pub_topic_name) {
            runtime::core::util::TopicMetaKey pub_key{
              .topic_name = pub_topic_name,
              .msg_type = key.msg_type,
            };
            const auto& topic_pub_wrapper = topic_pub_wrapper_map_.find(pub_key)->second;
            MW_REQUIRE(topic_pub_wrapper.send_type_wrapper_ptr, "Get publish type wrapper failed!");

            ibmw::transport::Context ctx;
            MessageFrame pub_msg_wrapper{
              .info = topic_pub_wrapper.send_type_wrapper_ptr->info,
              .msg_ptr = msg_wrapper.msg_ptr,
              .ctx_ref = ctx,
            };
            pub_msg_wrapper.serialization_cache = msg_wrapper.serialization_cache;

            core_ptr_->GetTransportBus().Send(std::move(pub_msg_wrapper));

          } });
        release_callback();
      };
      bool ret = core_ptr_->GetTransportBus().OnMessage(std::move(message_handler));
      MW_REQUIRE(ret, "Register subscribe transport failed!");
    }
  }
}

void ProxyExtension::RegisterPubTransport() {
  using namespace ibmw::runtime::core::transport;

  for (auto& proxy_action_itr : proxy_action_map_) {
    // register publish type
    auto& proxy_action = *(proxy_action_itr.second);

    const auto& topic_meta_map = proxy_action.GetTopicMetaMap();

    for (const auto& [_, topic_meta] : topic_meta_map) {
      auto finditr = type_support_map_.find(topic_meta.msg_type);
      MW_REQUIRE(finditr != type_support_map_.end(),
                              "Can not find type '{}' in any type support pkg!", topic_meta.msg_type);

      const auto& type_support_wrapper = finditr->second;

      // register publish type
      for (auto& pub_topic_name : topic_meta.pub_topic_name) {
        SendTypeWrapper send_type_wrapper;
        send_type_wrapper.info = TransportInfo{
            .msg_type = topic_meta.msg_type,
            .topic_name = pub_topic_name,
            .pkg_path = type_support_wrapper.options.path,
            .module_name = "core",
            .msg_type_support_ref = type_support_wrapper.type_support_ref};

        bool ret = core_ptr_->GetTransportBus().RegisterSendType(std::move(send_type_wrapper));
        MW_REQUIRE(ret, "Register publish type failed!");
      }
    }
    // map send_type_wrapper_ptr
    for (const auto& [_, topic_meta] : topic_meta_map) {
      for (auto& pub_topic_name : topic_meta.pub_topic_name) {
        runtime::core::util::TopicMetaKey key{
            .topic_name = pub_topic_name,
            .msg_type = topic_meta.msg_type};
        auto finditr = type_support_map_.find(topic_meta.msg_type);
        MW_REQUIRE(finditr != type_support_map_.end(),
                                "Can not find type '{}' in any type support pkg!", topic_meta.msg_type);

        const auto& type_support_wrapper = finditr->second;

        const auto* send_type_wrapper_ptr = core_ptr_->GetTransportBus().GetTransportCatalog()->GetSendTypeWrapperPtr(
            topic_meta.msg_type, pub_topic_name, type_support_wrapper.options.path, "core");

        MW_REQUIRE(send_type_wrapper_ptr, "Get publish type wrapper failed!");

        topic_pub_wrapper_map_.emplace(key, TopicPubWrapper{.send_type_wrapper_ptr = send_type_wrapper_ptr});
      }
    }
  }
}

void ProxyExtension::OnShutdown() {
  for (auto& proxy_action_itr : proxy_action_map_) {
    proxy_action_itr.second->Shutdown();
  }
}

}  // namespace ibmw::extensions::proxy_extension

IBMW_EXT_ENTRY(ibmw::extensions::proxy_extension::ProxyExtension)
