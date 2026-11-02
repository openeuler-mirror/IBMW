// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "mqtt_ext/mqtt_transport_driver.h"

#include <regex>

#include "cpp/util/type_support.h"
#include "mqtt_ext/global.h"
#include "base/buffer_util.h"
#include "base/url_encode.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::mqtt_extension::MqttTransportDriver::Options> {
  using Options = ibmw::extensions::mqtt_extension::MqttTransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["publishers"] = YAML::Node();
    for (const auto& pub_topic_options : rhs.pub_topics_options) {
      Node pub_topic_options_node;
      pub_topic_options_node["topic"] = pub_topic_options.topic_name;
      pub_topic_options_node["qos"] = pub_topic_options.qos;
      node["publishers"].push_back(pub_topic_options_node);
    }

    node["subscribers"] = YAML::Node();
    for (const auto& sub_topic_options : rhs.sub_topics_options) {
      Node sub_topic_options_node;
      sub_topic_options_node["topic"] = sub_topic_options.topic_name;
      sub_topic_options_node["qos"] = sub_topic_options.qos;
      node["subscribers"].push_back(sub_topic_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["publishers"] && node["publishers"].IsSequence()) {
      for (const auto& pub_topic_options_node : node["publishers"]) {
        int qos = 2;

        if (pub_topic_options_node["qos"]) qos = pub_topic_options_node["qos"].as<int>();
        MW_ASSERT(qos >= 0 && qos <= 2, "Invalid Mqtt qos: {}", qos);

        auto pub_topic_options = Options::PubTopicOptions{
            .topic_name = pub_topic_options_node["topic"].as<std::string>(),
            .qos = qos};

        rhs.pub_topics_options.emplace_back(std::move(pub_topic_options));
      }
    }

    if (node["subscribers"] && node["subscribers"].IsSequence()) {
      for (const auto& sub_topic_options_node : node["subscribers"]) {
        int qos = 2;

        if (sub_topic_options_node["qos"]) qos = sub_topic_options_node["qos"].as<int>();
        MW_ASSERT(qos >= 0 && qos <= 2, "Invalid Mqtt qos: {}", qos);

        auto sub_topic_options = Options::SubTopicOptions{
            .topic_name = sub_topic_options_node["topic"].as<std::string>(),
            .qos = qos};

        rhs.sub_topics_options.emplace_back(std::move(sub_topic_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::mqtt_extension {

void MqttTransportDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Mqtt transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void MqttTransportDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
  // The extension already waited (bounded) for the first connect attempt.
}

void MqttTransportDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  UnSubscribeMqttTopic();

  msg_handle_catalog_ptr_->Shutdown();
}

bool MqttTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    namespace util = ibmw::common::util;

    const auto& info = send_type_wrapper.info;

    int qos = 2;

    auto find_option_itr = std::find_if(
        options_.pub_topics_options.begin(), options_.pub_topics_options.end(),
        [topic_name = info.topic_name](const Options::PubTopicOptions& pub_option) {
          try {
            return std::regex_match(topic_name.begin(), topic_name.end(), std::regex(pub_option.topic_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       pub_option.topic_name, topic_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.pub_topics_options.end()) {
      qos = find_option_itr->qos;
    }

    pub_cfg_info_map_.emplace(
        info.topic_name,
        PubCfgInfo{
            .qos = qos});

    // Check path
    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    MW_REQUIRE(pattern.size() <= 255, "Too long uri: {}", pattern);

    MW_INFO("Register publish type to mqtt transport, url: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool MqttTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");
    // Checked before any side effect: a rejected registration must not
    // leave a handler entry or a subscription without a catalog handle.
    MW_REQUIRE(!msg_handle_catalog_ptr_->IsSealed(),
               "Mqtt subscriptions can only be registered before the engine starts.");

    namespace util = ibmw::common::util;

    const auto& info = message_handler.info;

    int qos = 2;

    auto find_option_itr = std::find_if(
        options_.sub_topics_options.begin(), options_.sub_topics_options.end(),
        [topic_name = info.topic_name](const Options::SubTopicOptions& sub_option) {
          try {
            return std::regex_match(topic_name.begin(), topic_name.end(), std::regex(sub_option.topic_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       sub_option.topic_name, topic_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.sub_topics_options.end()) {
      qos = find_option_itr->qos;
    }

    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    auto find_itr = message_handler_map_.find(pattern);
    if (find_itr != message_handler_map_.end()) {
      find_itr->second->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    auto msg_handler_tool_unique_ptr = std::make_unique<ibmw::runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

    auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();

    message_handler_map_.emplace(pattern, std::move(msg_handler_tool_unique_ptr));

    auto handle =
        [this, topic_name = info.topic_name, msg_handler_tool_ptr](MQTTAsync_message* message) {
          auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

          // parsing mqtt package
          util::ConstBufferOperator buf_oper(static_cast<const char*>(message->payload), message->payloadlen);

          std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
          ctx_ptr->SetSerializationType(serialization_type);

          // Get context
          size_t ctx_num = buf_oper.GetUint8();
          for (size_t ii = 0; ii < ctx_num; ++ii) {
            auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
            auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
            ctx_ptr->SetMetaValue(key, val);
          }

          ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, Name());

          // Get message buf
          auto remaining_buf = buf_oper.GetRemainingBuffer();

          msg_handler_tool_ptr->DoMessageCallback(
              ctx_ptr, serialization_type, static_cast<const void*>(remaining_buf.data()), remaining_buf.size());
        };

    msg_handle_catalog_ptr_->RegisterMsgHandle(pattern, std::move(handle));

    sub_info_vec_.emplace_back(MqttSubInfo{pattern, qos});

    MW_INFO("Register mqtt handle for transport, uri '{}'", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void MqttTransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    namespace util = ibmw::common::util;

    const auto& info = msg_wrapper.info;

    int qos = 2;

    auto find_itr = pub_cfg_info_map_.find(info.topic_name);
    if (find_itr != pub_cfg_info_map_.end()) {
      qos = find_itr->second.qos;
    }

    // Determine the data serialization type, first look for ctx. If it is not configured in ctx, use the first supported serialization type.
    auto publish_type_support_ref = info.msg_type_support_ref;

    // The context is optional: without one, no meta data is sent.
    std::string_view serialization_type;
    if (msg_wrapper.ctx_ref) {
      serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    }
    if (serialization_type.empty()) {
      serialization_type = publish_type_support_ref.DefaultSerializationType();
    }

    // msg serialization
    auto buffer_array_view_ptr = ibmw::runtime::core::transport::SerializeMsgWithCache(msg_wrapper, serialization_type);
    MW_REQUIRE(
        buffer_array_view_ptr,
        "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, topic_name: {}, msg_type: {}",
        serialization_type, info.pkg_path, info.module_name, info.topic_name, info.msg_type);

    // Fill the content
    const auto* buffer_array_data = buffer_array_view_ptr->Data();
    const size_t buffer_array_len = buffer_array_view_ptr->Size();
    size_t msg_size = buffer_array_view_ptr->BufferSize();

    // context
    ibmw_string_view_array_t meta_key_vals{};
    if (msg_wrapper.ctx_ref) {
      meta_key_vals = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
    }
    const auto* meta_key_vals_array = meta_key_vals.str_array;
    const size_t meta_key_vals_array_len = meta_key_vals.len;
    MW_REQUIRE(meta_key_vals_array_len / 2 <= 255,
                            "Too much context meta, require less than 255, but actually {}.", meta_key_vals_array_len / 2);

    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      context_meta_kv_size += (2 + meta_key_vals_array[ii].len);
    }

    size_t mqtt_pkg_size = 1 + serialization_type.size() + context_meta_kv_size + msg_size;

    MW_REQUIRE(mqtt_pkg_size <= max_pkg_size_,
                            "Mqtt publish failed, pkg is too large, limit {} bytes, actual {} bytes",
                            max_pkg_size_, mqtt_pkg_size);

    std::vector<char> msg_buf_vec(mqtt_pkg_size);

    util::BufferOperator buf_oper(msg_buf_vec.data(), msg_buf_vec.size());

    buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

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

    MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
    pubmsg.payload = msg_buf_vec.data();
    pubmsg.payloadlen = static_cast<int>(msg_buf_vec.size());  // <= max_pkg_size_ <= INT_MAX
    pubmsg.qos = qos;
    pubmsg.retained = 0;

    // Set url
    std::string mqtt_pub_topic = std::string("/transport/") +
                                 util::UrlEncode(info.topic_name) + "/" +
                                 util::UrlEncode(info.msg_type);

    MW_TRACE("Mqtt publish to '{}'", mqtt_pub_topic);
    int rc = MQTTAsync_sendMessage(client_, mqtt_pub_topic.data(), &pubmsg, NULL);
    MW_CHECK_WARN(rc == MQTTASYNC_SUCCESS,
                     "publish mqtt msg failed, topic: {}, code: {}",
                     mqtt_pub_topic, rc);
    return;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

void MqttTransportDriver::SubscribeMqttTopic() {
  for (auto sub_info : sub_info_vec_) {
    // todo:Replace with MQTTClient_subscribeMany
    int rc = MQTTAsync_subscribe(client_, sub_info.topic.data(), sub_info.qos, NULL);
    if (rc != MQTTASYNC_SUCCESS) {
      MW_ERROR("Failed to subscribe mqtt, topic: {} return code: {}", sub_info.topic, rc);
    }
  }
}

void MqttTransportDriver::UnSubscribeMqttTopic() {
  // todo:Replace with MQTTClient_unsubscribeMany
  for (const auto& sub_info : sub_info_vec_) {
    int rc = MQTTAsync_unsubscribe(client_, sub_info.topic.data(), NULL);
    if (rc != MQTTASYNC_SUCCESS) {
      MW_WARN("Failed to unsubscribe mqtt, topic: {} return code: {}", sub_info.topic, rc);
    }
  }
}
}  // namespace ibmw::extensions::mqtt_extension
