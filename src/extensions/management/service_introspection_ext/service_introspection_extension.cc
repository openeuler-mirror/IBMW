// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "service_introspection_ext/service_introspection_extension.h"
#include "c/service/rpc_context_base.h"
#include "cpp/util/json_type_support.h"
#include "engine/engine.h"
#include "engine/service/service_driver_tools.h"
#include "service_introspection_ext/global.h"
#include "service_introspection_ext/service_introspection_types.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::service_introspection_extension::ServiceIntrospectionExtension::Options> {
  using Options = ibmw::extensions::service_introspection_extension::ServiceIntrospectionExtension::Options;

  static Node encode(const Options &rhs) {
    Node node;

    node["client_info_topic_name"] = rhs.client_info_topic_name;
    node["server_info_topic_name"] = rhs.server_info_topic_name;

    if (rhs.mode == Options::Mode::kMeta) {
      node["mode"] = "meta";
    } else if (rhs.mode == Options::Mode::khybrid) {
      node["mode"] = "hybrid";
    } else if (rhs.mode == Options::Mode::kFull) {
      node["mode"] = "full";
    }

    if (rhs.rpc_serialization_type == Options::RpcSerializationType::kJson) {
      node["rpc_serialization_type"] = "json";
    } else if (rhs.rpc_serialization_type == Options::RpcSerializationType::kAuto) {
      node["rpc_serialization_type"] = "auto";
    }

    return node;
  }

  static bool decode(const Node &node, Options &rhs) {
    if (!node.IsMap()) return false;

    if (node["mode"]) {
      auto mode = ibmw::common::util::StrToLower(node["mode"].as<std::string>());
      if (mode == "meta") {
        rhs.mode = Options::Mode::kMeta;
      } else if (mode == "hybrid") {
        rhs.mode = Options::Mode::khybrid;
      } else if (mode == "full") {
        rhs.mode = Options::Mode::kFull;
      } else {
        throw ibmw::common::util::IbmwException("Invalid mode: " + mode);
      }
    }

    if (node["rpc_serialization_type"]) {
      auto rpc_serialization_type = ibmw::common::util::StrToLower(node["rpc_serialization_type"].as<std::string>());
      if (rpc_serialization_type == "json") {
        rhs.rpc_serialization_type = Options::RpcSerializationType::kJson;
      } else if (rpc_serialization_type == "auto") {
        rhs.rpc_serialization_type = Options::RpcSerializationType::kAuto;
      } else {
        throw ibmw::common::util::IbmwException("Invalid rpc serialization type: " + rpc_serialization_type);
      }
    }

    rhs.client_info_topic_name = node["client_info_topic_name"].as<std::string>();
    rhs.server_info_topic_name = node["server_info_topic_name"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::service_introspection_extension {

ServiceIntrospectionExtension::ServiceIntrospectionExtension() {
  SetExtensionLogger(&g_logger);
}

bool ServiceIntrospectionExtension::OnInit(runtime::core::Engine *core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  // Register filters
  RegisterClientFilter();
  RegisterServiceFilter();

  // Register publisher
  core_ptr_->RegisterHook(runtime::core::After(runtime::core::Phase::kInitialize,
                                                runtime::core::Subsystem::kTransport),
                          [this] { RegisterPublisher(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void ServiceIntrospectionExtension::OnShutdown() {
  stop_flag_ = true;
}

void ServiceIntrospectionExtension::RegisterPublisher() {
  using RpcMsgList = ibmw::protocols::service_introspection::RpcMsgList;

  auto setup_publisher = [this](const std::string &topic_name)
      -> ibmw::transport::SenderRef {
    auto publisher = ibmw::transport::TransportRef(
                         core_ptr_->GetTransportBus().GetBusAccess().NativeHandle())
                         .GetPublisher(topic_name);

    MW_ASSERT(publisher, "Failed to get publisher for topic: {}", topic_name);

    bool ret = publisher.RegisterSendType(
        ibmw::json::GetJsonMessageTypeSupport<RpcMsgList>());
    MW_ASSERT(ret, "Register publish type failed for topic: {}", topic_name);

    return publisher;
  };

  // set client_info publisher
  publisher_ = setup_publisher(options_.client_info_topic_name);
  client_info_publisher_ = std::make_unique<ibmw::transport::SenderRef>(publisher_);

  // set server_info publisher
  if (options_.client_info_topic_name == options_.server_info_topic_name) {
    server_info_publisher_ = std::make_unique<ibmw::transport::SenderRef>(publisher_);
  } else {
    publisher2_ = setup_publisher(options_.server_info_topic_name);
    server_info_publisher_ = std::make_unique<ibmw::transport::SenderRef>(publisher2_);
  }
}

void ServiceIntrospectionExtension::RegisterClientFilter() {
  using RpcMsgList = ibmw::protocols::service_introspection::RpcMsgList;
  using RpcMsg = ibmw::protocols::service_introspection::RpcMsg;

  static const std::string kMsgTypeName =
      "json:" + std::string(RpcMsgList::kTypeName);

  core_ptr_->GetServiceDispatcher().RegisterClientFilter("service_introspection", [this](const std::shared_ptr<ibmw::runtime::core::service::ServiceMessageFrame> &wrapper_ptr,
                                                                                  ibmw::runtime::core::service::FrameworkAsyncRpcHandle &&h) {
    wrapper_ptr->callback =
        [this, wrapper_ptr, callback{std::move(wrapper_ptr->callback)}](ibmw::rpc::Status status) {
          // fill protocol
          RpcMsgList rpc_msg_list;
          RpcMsg rpc_msg;

          // fill metadata
          rpc_msg.event_type = ibmw::protocols::service_introspection::ServiceEventType::kClient;
          rpc_msg.time_stamp = ibmw::common::util::GetCurTimestampNs();
          rpc_msg.service_name = wrapper_ptr->info.func_name;
          rpc_msg.module_name = wrapper_ptr->info.module_name;
          rpc_msg.driver_type = std::string(wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER));
          rpc_msg.sequence_number = std::stoull(std::string(wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_REQUEST_ID)));
          rpc_msg.status_code = status.Code();

          std::string_view rpc_serialization_type;
          if (options_.rpc_serialization_type == Options::RpcSerializationType::kJson) {
            rpc_serialization_type = "json";
          } else {
            rpc_serialization_type = wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);
          }

          if (options_.mode != Options::Mode::kMeta) {  // fill request and response
            auto req_str = TrySerializeReqWithCache(*wrapper_ptr, rpc_serialization_type)->JoinToCharVector();
            auto rsp_str = TrySerializeRspWithCache(*wrapper_ptr, rpc_serialization_type)->JoinToCharVector();
            rpc_msg.request = std::string(req_str.data(), req_str.size());
            rpc_msg.response = std::string(rsp_str.data(), rsp_str.size());
          }

          rpc_msg_list.rpc_msg.emplace_back(std::move(rpc_msg));

          // publish
          ibmw::transport::Context ctx;
          ctx.SetSerializationType("json");
          client_info_publisher_->Send(kMsgTypeName, ctx, static_cast<const void*>(&rpc_msg_list));

          // call user's callback
          callback(status);
        };

    h(wrapper_ptr);
  });
}

void ServiceIntrospectionExtension::RegisterServiceFilter() {
  using RpcMsgList = ibmw::protocols::service_introspection::RpcMsgList;
  using RpcMsg = ibmw::protocols::service_introspection::RpcMsg;

  static const std::string kMsgTypeName =
      "json:" + std::string(RpcMsgList::kTypeName);

  core_ptr_->GetServiceDispatcher().RegisterServerFilter("service_introspection", [this](const std::shared_ptr<ibmw::runtime::core::service::ServiceMessageFrame> &wrapper_ptr,
                                                                                  ibmw::runtime::core::service::FrameworkAsyncRpcHandle &&h) {
    wrapper_ptr->callback =
        [this, wrapper_ptr, callback{std::move(wrapper_ptr->callback)}](ibmw::rpc::Status status) {
          RpcMsgList rpc_msg_list;
          RpcMsg rpc_msg;

          // fill metadata
          rpc_msg.event_type = ibmw::protocols::service_introspection::ServiceEventType::kServer;
          rpc_msg.time_stamp = ibmw::common::util::GetCurTimestampNs();
          rpc_msg.service_name = wrapper_ptr->info.func_name;
          rpc_msg.module_name = wrapper_ptr->info.module_name;
          rpc_msg.driver_type = std::string(wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER));
          rpc_msg.sequence_number = std::stoull(std::string(wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_RESPONSE_ID)));
          rpc_msg.status_code = status.Code();

          std::string_view rpc_serialization_type;
          if (options_.rpc_serialization_type == Options::RpcSerializationType::kJson) {
            rpc_serialization_type = "json";
          } else {
            rpc_serialization_type = wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);
          }

          if (options_.mode == Options::Mode::kFull) {  // fill request and response
            auto req_str = TrySerializeReqWithCache(*wrapper_ptr, rpc_serialization_type)->JoinToCharVector();
            auto rsp_str = TrySerializeRspWithCache(*wrapper_ptr, rpc_serialization_type)->JoinToCharVector();
            rpc_msg.request = std::string(req_str.data(), req_str.size());
            rpc_msg.response = std::string(rsp_str.data(), rsp_str.size());
          }

          rpc_msg_list.rpc_msg.emplace_back(std::move(rpc_msg));

          // publish
          ibmw::transport::Context ctx;
          ctx.SetSerializationType("json");
          server_info_publisher_->Send(kMsgTypeName, ctx, static_cast<const void*>(&rpc_msg_list));

          // call user's callback
          callback(status);
        };

    h(wrapper_ptr);
  });
}

}  // namespace ibmw::extensions::service_introspection_extension

IBMW_EXT_ENTRY(ibmw::extensions::service_introspection_extension::ServiceIntrospectionExtension)
