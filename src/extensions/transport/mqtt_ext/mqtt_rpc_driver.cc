// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "mqtt_ext/mqtt_rpc_driver.h"

#include <optional>
#include <regex>

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_status.h"
#include "cpp/util/type_support.h"
#include "engine/service/service_driver_tools.h"
#include "mqtt_ext/global.h"
#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "base/url_parser.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::mqtt_extension::MqttRpcDriver::Options> {
  using Options = ibmw::extensions::mqtt_extension::MqttRpcDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["timeout_executor"] = rhs.timeout_executor;

    node["clients"] = YAML::Node();
    for (const auto& client_options : rhs.clients_options) {
      Node client_options_node;
      client_options_node["func_name"] = client_options.func_name;
      client_options_node["server_mqtt_id"] = client_options.server_mqtt_id;
      client_options_node["qos"] = client_options.qos;
      node["clients"].push_back(client_options_node);
    }

    node["servers"] = YAML::Node();
    for (const auto& server_options : rhs.servers_options) {
      Node server_options_node;
      server_options_node["func_name"] = server_options.func_name;
      server_options_node["allow_share"] = server_options.allow_share;
      server_options_node["qos"] = server_options.qos;
      node["servers"].push_back(server_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["timeout_executor"])
      rhs.timeout_executor = node["timeout_executor"].as<std::string>();

    if (node["clients"] && node["clients"].IsSequence()) {
      for (const auto& client_options_node : node["clients"]) {
        auto client_options = Options::ClientOptions{
            .func_name = client_options_node["func_name"].as<std::string>()};

        if (client_options_node["server_mqtt_id"])
          client_options.server_mqtt_id = client_options_node["server_mqtt_id"].as<std::string>();

        int qos = 2;
        if (client_options_node["qos"]) qos = client_options_node["qos"].as<int>();
        MW_ASSERT(qos >= 0 && qos <= 2, "Invalid Mqtt qos: {}", qos);
        client_options.qos = qos;

        rhs.clients_options.emplace_back(std::move(client_options));
      }
    }

    if (node["servers"] && node["servers"].IsSequence()) {
      for (const auto& server_options_node : node["servers"]) {
        auto server_options = Options::ServerOptions{
            .func_name = server_options_node["func_name"].as<std::string>()};

        if (server_options_node["allow_share"])
          server_options.allow_share = server_options_node["allow_share"].as<bool>();

        int qos = 2;
        if (server_options_node["qos"]) qos = server_options_node["qos"].as<int>();
        MW_ASSERT(qos >= 0 && qos <= 2, "Invalid Mqtt qos: {}", qos);

        server_options.qos = qos;

        rhs.servers_options.emplace_back(std::move(server_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::mqtt_extension {

namespace {
constexpr std::string_view kRspTopicPrefix = "ibmw_rpc_rsp/";
}  // namespace

void MqttRpcDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Mqtt Service driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  client_tool_ptr_ =
      std::make_unique<runtime::core::util::RpcClientTool<std::shared_ptr<runtime::core::service::ServiceMessageFrame>>>();

  if (!options_.timeout_executor.empty()) {
    MW_REQUIRE(
        get_executor_func_,
        "Get executor function is not set before initialize.");

    auto timeout_executor = get_executor_func_(options_.timeout_executor);

    MW_REQUIRE(
        timeout_executor,
        "Get timeout executor '{}' failed.", options_.timeout_executor);

    client_tool_ptr_->RegisterTimeoutExecutor(timeout_executor);
    client_tool_ptr_->RegisterTimeoutHandle(
        [](auto&& client_call_wrapper_ptr) {
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_TIMEOUT));
        });

    MW_TRACE("Mqtt service driver enable the timeout function, use '{}' as timeout executor.",
                options_.timeout_executor);
  } else {
    MW_TRACE("Mqtt service driver does not enable the timeout function.");
  }

  // Use a non-shared subscription: every runtime answers on the querying
  // client's unique reply topic. Nothing is retained at the broker.
  namespace directory = ibmw::discovery::service_protocol;
  sub_info_vec_.push_back({directory::kMqttRequestTopic, 1});
  msg_handle_catalog_ptr_->RegisterMsgHandle(directory::kMqttRequestTopic,
      [catalog = discovery_catalog_, client = client_](MQTTAsync_message* message) {
        try {
          constexpr std::string_view prefix = directory::kMqttReplyPrefix;
          if (message->retained || message->payloadlen != static_cast<int>(prefix.size() + 20)) return;
          const std::string topic(static_cast<const char*>(message->payload), message->payloadlen);
          if (!topic.starts_with(prefix) ||
              topic.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos) return;
          const auto payload = catalog->Encode();
          MQTTAsync_message reply = MQTTAsync_message_initializer;
          reply.payload = const_cast<char*>(payload.data());
          reply.payloadlen = static_cast<int>(payload.size());
          reply.qos = 1;
          const auto code = MQTTAsync_sendMessage(client, topic.c_str(), &reply, nullptr);
          // Requests come from any broker client; rate-limit so a flood cannot flood the log.
          if (code != MQTTASYNC_SUCCESS)
            MW_WARN_INTERVAL(1000, "Cannot send MQTT service directory: {}", code);
        } catch (const std::exception& error) {
          MW_WARN_INTERVAL(1000, "Cannot produce MQTT service directory: {}", error.what());
        } catch (...) {
          MW_WARN_INTERVAL(1000, "Cannot produce MQTT service directory: unknown exception");
        }
      });

  options_node = options_;
}

void MqttRpcDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
  // The extension already waited (bounded) for the first connect attempt.
}
void MqttRpcDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  UnSubscribeMqttTopic();

  // Wait for in-flight response handlers before releasing the client tool
  // they use (TRANS-015). The catalog is shared with the transport driver:
  // whichever shuts down first stops intake for both, so answers arriving
  // after that end their calls by timeout.
  msg_handle_catalog_ptr_->Shutdown();

  // client_tool_ptr_ stays until destruction: Call() and late handlers may
  // still reach it after Shutdown. Calls still waiting then get their result
  // from the destructor.
}

MqttRpcDriver::~MqttRpcDriver() {
  if (!client_tool_ptr_) return;
  client_tool_ptr_->AbandonAll([](auto&& client_call_wrapper_ptr) {
    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  });
}

bool MqttRpcDriver::RegisterServiceFunc(
    const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Service func can only be registered when state is 'Init'.");
      return false;
    }
    // Before any side effect: no subscription without a catalog handle.
    if (msg_handle_catalog_ptr_->IsSealed()) {
      MW_ERROR("Service func can only be registered before the engine starts.");
      return false;
    }

    namespace util = ibmw::common::util;

    const auto& info = service_func_wrapper.info;

    bool allow_share = true;
    int qos = 2;

    auto find_option_itr = std::find_if(
        options_.servers_options.begin(), options_.servers_options.end(),
        [func_name = info.func_name](const Options::ServerOptions& server_option) {
          try {
            auto real_func_name = std::string(rpc::GetFuncNameWithoutPrefix(func_name));
            return std::regex_match(func_name.begin(), func_name.end(),
                                    std::regex(server_option.func_name, std::regex::ECMAScript)) ||
                   std::regex_match(real_func_name.begin(), real_func_name.end(),
                                    std::regex(server_option.func_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       server_option.func_name, func_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.servers_options.end()) {
      allow_share = find_option_itr->allow_share;
      qos = find_option_itr->qos;
    }

    auto handle = [this, qos, &service_func_wrapper](MQTTAsync_message* message) {
      try {
        // Create a service invoke wrapper
        auto service_invoke_wrapper_ptr = std::make_shared<runtime::core::service::ServiceMessageFrame>(
            runtime::core::service::ServiceMessageFrame{.info = service_func_wrapper.info});
        const auto& info = service_invoke_wrapper_ptr->info;

        // Create service ctx
        auto ctx_ptr = std::make_shared<ibmw::rpc::Context>(ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT);
        service_invoke_wrapper_ptr->ctx_ref = ctx_ptr;

        // Get fields
        util::ConstBufferOperator buf_oper(static_cast<const char*>(message->payload), message->payloadlen);

        std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
        ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, serialization_type);

        std::string mqtt_pub_topic(buf_oper.GetString(util::BufferLenType::kUInt8));
        // Answers only go to client answer topics, never to a topic of the
        // peer's choice (e.g. a data or request topic).
        if (!mqtt_pub_topic.starts_with(kRspTopicPrefix)) [[unlikely]] {
          MW_WARN_INTERVAL(5000, "Drop rpc request for '{}': answer topic must start with '{}'.",
                           info.func_name, kRspTopicPrefix);
          return;
        }

        char req_id_buf[4];
        buf_oper.GetBuffer(req_id_buf, 4);

        // Get context
        size_t ctx_num = buf_oper.GetUint8();
        for (size_t ii = 0; ii < ctx_num; ++ii) {
          auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
          auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
          ctx_ptr->SetMetaValue(key, val);
        }

        ctx_ptr->SetFunctionName(info.func_name);
        ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER, Name());

        // service req deserialization
        auto remaining_buf = buf_oper.GetRemainingBuffer();
        ibmw_buffer_view_t buffer_view{
            .data = remaining_buf.data(),
            .len = remaining_buf.size()};

        ibmw_buffer_array_view_t buffer_array_view{
            .data = &buffer_view,
            .len = 1};

        std::shared_ptr<void> service_req_ptr = info.req_type_support_ref.CreateSharedPtr();
        service_invoke_wrapper_ptr->req_ptr = service_req_ptr.get();

        bool deserialize_ret = info.req_type_support_ref.Deserialize(
            serialization_type, buffer_array_view, service_req_ptr.get());

        if (!deserialize_ret) [[unlikely]] {
          MW_ERROR("Mqtt req deserialize failed.");

          ReturnRspWithStatusCode(
              mqtt_pub_topic, qos, serialization_type, req_id_buf, IBMW_RPC_STATUS_SVR_DESERIALIZATION_FAILED);

          return;
        }

        // Create service rsp
        std::shared_ptr<void> service_rsp_ptr = info.rsp_type_support_ref.CreateSharedPtr();
        service_invoke_wrapper_ptr->rsp_ptr = service_rsp_ptr.get();

        // Set callback
        service_invoke_wrapper_ptr->callback =
            [this,
             service_invoke_wrapper_ptr,
             qos,
             ctx_ptr,
             service_req_ptr,
             service_rsp_ptr,
             serialization_type{std::move(serialization_type)},
             mqtt_pub_topic{std::move(mqtt_pub_topic)},
             req_id_buf](ibmw::rpc::Status status) {
              if (!status.OK()) [[unlikely]] {
                // If the code is not suc, then deserialization is not necessary
                ReturnRspWithStatusCode(
                    mqtt_pub_topic, qos, serialization_type, req_id_buf, status.Code());

                return;
              }

              // service rsp serialization
              auto buffer_array_view_ptr = ibmw::runtime::core::service::TrySerializeRspWithCache(*service_invoke_wrapper_ptr, serialization_type);
              if (!buffer_array_view_ptr) [[unlikely]] {
                ReturnRspWithStatusCode(
                    mqtt_pub_topic, qos, serialization_type, req_id_buf, IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED);

                return;
              }

              const auto* buffer_array_data = buffer_array_view_ptr->Data();
              const size_t buffer_array_len = buffer_array_view_ptr->Size();
              size_t rsp_size = buffer_array_view_ptr->BufferSize();

              size_t mqtt_pkg_size = 1 + serialization_type.size() + 4 + 4 + rsp_size;

              if (mqtt_pkg_size > max_pkg_size_) [[unlikely]] {
                MW_WARN("Mqtt publish failed, pkg is too large, limit {}k, actual {}k",
                           max_pkg_size_ / 1024, mqtt_pkg_size / 1024);

                ReturnRspWithStatusCode(
                    mqtt_pub_topic, qos, serialization_type, req_id_buf, IBMW_RPC_STATUS_SVR_UNKNOWN);
                return;
              }

              std::vector<char> msg_buf_vec(mqtt_pkg_size);
              util::BufferOperator buf_oper(msg_buf_vec.data(), msg_buf_vec.size());

              buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);
              buf_oper.SetBuffer(req_id_buf, sizeof(req_id_buf));
              buf_oper.SetUint32(0);

              for (size_t ii = 0; ii < buffer_array_len; ++ii) {
                buf_oper.SetBuffer(
                    static_cast<const char*>(buffer_array_data[ii].data),
                    buffer_array_data[ii].len);
              }

              MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
              pubmsg.payload = msg_buf_vec.data();
              pubmsg.payloadlen = static_cast<int>(msg_buf_vec.size());  // <= max_pkg_size_ <= INT_MAX
              pubmsg.qos = qos;
              pubmsg.retained = 0;

              MW_TRACE("Mqtt publish to '{}'", mqtt_pub_topic);
              int rc = MQTTAsync_sendMessage(client_, mqtt_pub_topic.data(), &pubmsg, NULL);
              MW_CHECK_WARN(rc == MQTTASYNC_SUCCESS,
                               "publish mqtt msg failed, topic: {}, code: {}",
                               mqtt_pub_topic, rc);
            };

        // Call service rpc
        service_func_wrapper.service_func(service_invoke_wrapper_ptr);

      } catch (const std::exception& e) {
        MW_WARN("Handle mqtt rpc msg failed, exception info: {}", e.what());
      }
    };

    // check allow_share
    if (allow_share) {
      std::string mqtt_sub_topic = "ibmw_rpc_req/" + util::UrlEncode(rpc::GetFuncNameWithoutPrefix(info.func_name));
      std::string share_mqtt_sub_topic = "$share/ibmw/" + mqtt_sub_topic;
      sub_info_vec_.emplace_back(MqttSubInfo{share_mqtt_sub_topic, qos});

      msg_handle_catalog_ptr_->RegisterMsgHandle(mqtt_sub_topic, handle);
    }

    std::string mqtt_sub_topic_2 =
        "ibmw_rpc_req/" +
        util::UrlEncode(client_id_) + "/" +
        util::UrlEncode(rpc::GetFuncNameWithoutPrefix(info.func_name));
    sub_info_vec_.emplace_back(MqttSubInfo{mqtt_sub_topic_2, qos});

    msg_handle_catalog_ptr_->RegisterMsgHandle(mqtt_sub_topic_2, handle);
    discovery_catalog_->Add(info.func_name, info.req_type_support_ref.TypeName(),
                            info.rsp_type_support_ref.TypeName());

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool MqttRpcDriver::RegisterClientFunc(
    const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Client func can only be registered when state is 'Init'.");
      return false;
    }
    // Before any side effect: no subscription without a catalog handle.
    if (msg_handle_catalog_ptr_->IsSealed()) {
      MW_ERROR("Client func can only be registered before the engine starts.");
      return false;
    }

    namespace util = ibmw::common::util;

    const auto& info = client_func_wrapper.info;

    std::string server_mqtt_id;
    int qos = 2;

    auto find_option_itr = std::find_if(
        options_.clients_options.begin(), options_.clients_options.end(),
        [func_name = info.func_name](const Options::ClientOptions& client_option) {
          try {
            auto real_func_name = std::string(rpc::GetFuncNameWithoutPrefix(func_name));
            return std::regex_match(func_name.begin(), func_name.end(),
                                    std::regex(client_option.func_name, std::regex::ECMAScript)) ||
                   std::regex_match(real_func_name.begin(), real_func_name.end(),
                                    std::regex(client_option.func_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       client_option.func_name, func_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.clients_options.end()) {
      server_mqtt_id = find_option_itr->server_mqtt_id;
      qos = find_option_itr->qos;
    }

    client_cfg_info_map_.emplace(
        rpc::GetFuncNameWithoutPrefix(info.func_name),
        ClientCfgInfo{
            .server_mqtt_id = server_mqtt_id,
            .qos = qos});

    std::string mqtt_sub_topic =
        "ibmw_rpc_rsp/" +
        util::UrlEncode(client_id_) + "/" +
        util::UrlEncode(rpc::GetFuncNameWithoutPrefix(info.func_name));

    if (mqtt_sub_topic.size() > 255) {
      MW_ERROR("Too long mqtt topic name: {}", mqtt_sub_topic);
      return false;
    }

    sub_info_vec_.emplace_back(MqttSubInfo{mqtt_sub_topic, qos});

    msg_handle_catalog_ptr_->RegisterMsgHandle(
        mqtt_sub_topic,
        [this, func = std::string(rpc::GetFuncNameWithoutPrefix(info.func_name))](MQTTAsync_message* message) {
          std::shared_ptr<runtime::core::service::ServiceMessageFrame> client_call_wrapper_ptr;

          try {
            util::ConstBufferOperator buf_oper(static_cast<const char*>(message->payload), message->payloadlen);

            std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
            uint32_t req_id = buf_oper.GetUint32();
            uint32_t code = buf_oper.GetUint32();

            // Only an answer on the topic of the called function completes
            // the call (the topic is part of what the client asked for).
            auto msg_recorder = client_tool_ptr_->GetRecordIf(req_id, [&func](const auto& record) {
              return rpc::GetFuncNameWithoutPrefix(record->info.func_name) == func;
            });
            if (!msg_recorder) [[unlikely]] {
              // No record is found, which means that the call has timed out. The record has been deleted after the timeout process is gone.
              MW_TRACE("Can not get req id {} from recorder.", req_id);
              return;
            }

            // Get the record
            client_call_wrapper_ptr = std::move(*msg_recorder);

            if (code) [[unlikely]] {
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(code));
              return;
            }

            const auto& info = client_call_wrapper_ptr->info;

            // client rsp deserialization
            auto remaining_buf = buf_oper.GetRemainingBuffer();
            ibmw_buffer_view_t buffer_view{
                .data = remaining_buf.data(),
                .len = remaining_buf.size()};

            ibmw_buffer_array_view_t buffer_array_view{
                .data = &buffer_view,
                .len = 1};

            bool deserialize_ret = info.rsp_type_support_ref.Deserialize(
                serialization_type, buffer_array_view, client_call_wrapper_ptr->rsp_ptr);

            if (!deserialize_ret) {
              // Deserialization failed
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DESERIALIZATION_FAILED));
              return;
            }

            CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_OK));
            return;

          } catch (const std::exception& e) {
            MW_WARN("Handle mqtt rpc msg failed, exception info: {}", e.what());
          }

          if (client_call_wrapper_ptr)
            CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
        });

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool MqttRpcDriver::IsClientReady(std::string_view, std::string_view function) const noexcept {
  try {
    const auto name = rpc::GetFuncNameWithoutPrefix(function);
    if (state_.load() != State::kStart || !MQTTAsync_isConnected(client_) ||
        !client_cfg_info_map_.contains(name)) return false;
    const auto subscriptions = std::atomic_load(&subscriptions_);
    if (!subscriptions) return false;
    const auto topic = "ibmw_rpc_rsp/" + common::util::UrlEncode(client_id_) + "/" +
                       common::util::UrlEncode(name);
    const auto subscription = subscriptions->find(topic);
    // Completion publishes the failure flag; read it after pending reaches zero.
    return subscription != subscriptions->end() && subscription->second->pending.load() == 0 &&
           !subscription->second->failed.load();
  } catch (...) { return false; }
}

void MqttRpcDriver::Call(
    const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept {
  // Once the call is recorded, a failure must claim the record first so the
  // result is delivered exactly once, whichever of the answer, the timeout or
  // this path gets there first.
  std::optional<uint32_t> recorded_req_id;
  // Also used from the catch blocks of this noexcept function: a throwing
  // client callback must not terminate the process.
  auto fail = [&](uint32_t code) noexcept {
    try {
      if (!recorded_req_id) {
        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(code));
        return;
      }
      if (auto record = client_tool_ptr_->GetRecord(*recorded_req_id))
        CallResultCallback(**record, ibmw::rpc::Status(code));
    } catch (...) {
      try {
        MW_ERROR("Client callback threw while reporting a failed call.");
      } catch (...) {
      }
    }
  };

  try {
    if (state_.load() != State::kStart) [[unlikely]] {
      MW_WARN("Method can only be called when state is 'Start'.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    namespace util = ibmw::common::util;

    const auto& info = client_call_wrapper_ptr->info;

    auto real_func_name = rpc::GetFuncNameWithoutPrefix(info.func_name);

    std::string server_mqtt_id;
    int qos = 2;

    auto find_itr = client_cfg_info_map_.find(real_func_name);
    if (find_itr != client_cfg_info_map_.end()) {
      server_mqtt_id = find_itr->second.server_mqtt_id;
      qos = find_itr->second.qos;
    }

    // url: mqtt://server_mqtt_id
    auto to_addr = client_call_wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR);
    if (!to_addr.empty()) {
      auto url = util::ParseUrl<std::string_view>(to_addr);
      if (url) {
        if (url->protocol != Name()) [[unlikely]] {
          MW_WARN("Invalid addr: {}", to_addr);
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
          return;
        }
        server_mqtt_id = url->host;
      }
    }

    uint32_t cur_req_id = req_id_++;

    auto serialization_type =
        client_call_wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);

    if (serialization_type.size() > 255) [[unlikely]] {
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_UNKNOWN));
      return;
    }

    // ibmw_rpc_rsp/client_id/uri
    std::string mqtt_sub_topic =
        "ibmw_rpc_rsp/" +
        util::UrlEncode(client_id_) + "/" +
        util::UrlEncode(real_func_name);

    if (mqtt_sub_topic.size() > 255) [[unlikely]] {
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_UNKNOWN));
      return;
    }

    // client req serialization
    auto buffer_array_view_ptr = ibmw::runtime::core::service::TrySerializeReqWithCache(*client_call_wrapper_ptr, serialization_type);
    if (!buffer_array_view_ptr) [[unlikely]] {
      // Serialization failed
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED));
      return;
    }

    const auto* buffer_array_data = buffer_array_view_ptr->Data();
    const size_t buffer_array_len = buffer_array_view_ptr->Size();
    size_t req_size = buffer_array_view_ptr->BufferSize();

    // context
    client_call_wrapper_ptr->ctx_ref.SetMetaValue("ibmw-from_mqtt_client", client_id_);
    auto [meta_key_vals_array, meta_key_vals_array_len] = client_call_wrapper_ptr->ctx_ref.GetMetaKeyValsArray();
    if (meta_key_vals_array_len / 2 > 255) [[unlikely]] {
      MW_WARN("Too much context meta, require less than 255, but actually {}.", meta_key_vals_array_len / 2);
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      context_meta_kv_size += (2 + meta_key_vals_array[ii].len);
    }

    // Fill mqtt package
    size_t mqtt_pkg_size = 1 + serialization_type.size() +
                           1 + mqtt_sub_topic.size() +
                           4 +
                           context_meta_kv_size +
                           req_size;

    if (mqtt_pkg_size > max_pkg_size_) [[unlikely]] {
      MW_WARN("Mqtt publish failed, pkg is too large, limit {}k, actual {}k",
                 max_pkg_size_ / 1024, mqtt_pkg_size / 1024);

      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_SEND_REQ_FAILED));
      return;
    }

    // Record the callbacks and other contents
    auto timeout = client_call_wrapper_ptr->ctx_ref.Timeout();
    bool ret = client_tool_ptr_->Record(cur_req_id, timeout, std::shared_ptr(client_call_wrapper_ptr));
    if (!ret) [[unlikely]] {
      ret = client_tool_ptr_->RetryRecord(
          cur_req_id, timeout, client_call_wrapper_ptr, [this] { return req_id_++; });
    }

    if (!ret) [[unlikely]] {
      MW_ERROR("Failed to record msg.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }
    recorded_req_id = cur_req_id;
    // Fast fail for a call that raced Shutdown(): it would only wait for an
    // answer that can no longer be delivered. A call that slips past this
    // check is completed by its timeout or by the destructor.
    if (state_.load() != State::kStart) [[unlikely]] {
      fail(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR);
      return;
    }

    std::vector<char> msg_buf_vec(mqtt_pkg_size);

    util::BufferOperator buf_oper(msg_buf_vec.data(), msg_buf_vec.size());

    buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);
    buf_oper.SetString(mqtt_sub_topic, util::BufferLenType::kUInt8);
    buf_oper.SetUint32(cur_req_id);

    buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
    }

    // data
    for (size_t ii = 0; ii < buffer_array_len; ++ii) {
      buf_oper.SetBuffer(
          static_cast<const char*>(buffer_array_data[ii].data),
          buffer_array_data[ii].len);
    }

    // send
    MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
    pubmsg.payload = msg_buf_vec.data();
    pubmsg.payloadlen = static_cast<int>(msg_buf_vec.size());  // <= max_pkg_size_ <= INT_MAX
    pubmsg.qos = qos;
    pubmsg.retained = 0;

    // topic: ibmw_rpc_req/uri
    std::string mqtt_pub_topic;
    if (server_mqtt_id.empty()) {
      mqtt_pub_topic = "ibmw_rpc_req/" + util::UrlEncode(real_func_name);
    } else {
      mqtt_pub_topic =
          "ibmw_rpc_req/" +
          util::UrlEncode(server_mqtt_id) + "/" +
          util::UrlEncode(real_func_name);
    }

    MW_TRACE("Mqtt publish to '{}'", mqtt_pub_topic);
    int rc = MQTTAsync_sendMessage(client_, mqtt_pub_topic.data(), &pubmsg, NULL);
    if (rc != MQTTASYNC_SUCCESS) [[unlikely]] {
      MW_WARN("publish mqtt msg failed, topic: {}, code: {}", mqtt_pub_topic, rc);
      // Fail the call now instead of leaving it to the timeout (or forever
      // when no timeout executor is configured) (TRANS-014). The record may
      // already be gone if a timeout raced us.
      fail(IBMW_RPC_STATUS_CLI_SEND_REQ_FAILED);
    }
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    fail(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR);
  } catch (...) {
    MW_ERROR("Mqtt rpc call failed with a non-std exception.");
    fail(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR);
  }
}

void MqttRpcDriver::RegisterGetExecutorFunc(
    const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  get_executor_func_ = get_executor_func;
}

void MqttRpcDriver::SubscribeMqttTopic() {
  // Each reconnect owns a distinct batch. Late callbacks cannot mark a newer
  // connection ready, and callbacks never dereference a destroyed RPC driver.
  const auto batch = std::make_shared<SubscriptionBatch>();
  // Build the immutable topic map before publishing it to readiness readers.
  for (const auto& sub_info : sub_info_vec_) {
    auto& status = (*batch)[sub_info.topic];
    if (!status) status = std::make_shared<SubscriptionStatus>();
    status->pending.fetch_add(1);
  }
  std::atomic_store(&subscriptions_, batch);
  for (auto sub_info : sub_info_vec_) {
    // todo:Replace with MQTTClient_subscribeMany
    MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
    using Context = std::shared_ptr<SubscriptionStatus>;
    const auto& status = batch->at(sub_info.topic);
    response.context = new Context(status);
    response.onSuccess = [](void* context, MQTTAsync_successData* data) {
      const std::unique_ptr<Context> owner(static_cast<Context*>(context));
      if (!data || data->alt.qos == 128) (*owner)->failed.store(true);
      (*owner)->pending.fetch_sub(1);
    };
    response.onFailure = [](void* context, MQTTAsync_failureData*) {
      const std::unique_ptr<Context> owner(static_cast<Context*>(context));
      (*owner)->failed.store(true);
      (*owner)->pending.fetch_sub(1);
    };
    // A failure only blocks calls that need this subscription.
    int rc = MQTTAsync_subscribe(client_, sub_info.topic.data(), sub_info.qos, &response);
    if (rc != MQTTASYNC_SUCCESS) {
      delete static_cast<Context*>(response.context);
      status->failed.store(true);
      status->pending.fetch_sub(1);
      MW_ERROR("Failed to subscribe mqtt, topic: {} return code: {}", sub_info.topic, rc);
    }
  }
}

void MqttRpcDriver::UnSubscribeMqttTopic() {
  // todo:Replace with MQTTClient_unsubscribeMany
  for (const auto& sub_info : sub_info_vec_) {
    int rc = MQTTAsync_unsubscribe(client_, sub_info.topic.data(), NULL);
    if (rc != MQTTASYNC_SUCCESS) {
      MW_WARN("Failed to unsubscribe mqtt, topic: {} return code: {}", sub_info.topic, rc);
    }
  }
}

void MqttRpcDriver::ReturnRspWithStatusCode(
    const std::string& mqtt_pub_topic,
    int qos,
    std::string_view serialization_type,
    const char* req_id_buf,
    uint32_t code) {
  namespace util = ibmw::common::util;

  std::vector<char> msg_buf_vec(1 + serialization_type.size() + 4 + 4);
  util::BufferOperator buf_oper(msg_buf_vec.data(), msg_buf_vec.size());

  buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);
  buf_oper.SetBuffer(req_id_buf, 4);
  buf_oper.SetUint32(code);

  MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
  pubmsg.payload = msg_buf_vec.data();
  pubmsg.payloadlen = static_cast<int>(msg_buf_vec.size());
  pubmsg.qos = qos;
  pubmsg.retained = 0;

  MW_TRACE("Mqtt publish to '{}'", mqtt_pub_topic);
  int rc = MQTTAsync_sendMessage(client_, mqtt_pub_topic.data(), &pubmsg, NULL);
  MW_CHECK_WARN(rc == MQTTASYNC_SUCCESS,
                   "publish mqtt msg failed, topic: {}, code: {}",
                   mqtt_pub_topic, rc);
}

}  // namespace ibmw::extensions::mqtt_extension
