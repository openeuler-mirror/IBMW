// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "net_ext/udp/udp_transport_driver.h"

#include <regex>

#include "cpp/util/type_support.h"
#include "net_ext/global.h"
#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "base/url_parser.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::net_extension::UdpTransportDriver::Options> {
  using Options = ibmw::extensions::net_extension::UdpTransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["publishers"] = YAML::Node();
    for (const auto& pub_topic_options : rhs.pub_topics_options) {
      Node pub_topic_options_node;
      pub_topic_options_node["topic"] = pub_topic_options.topic_name;
      pub_topic_options_node["server_url_list"] = pub_topic_options.server_url_list;
      node["publishers"].push_back(pub_topic_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["publishers"] && node["publishers"].IsSequence()) {
      for (const auto& pub_topic_options_node : node["publishers"]) {
        auto pub_topic_options = Options::PubTopicOptions{
            .topic_name = pub_topic_options_node["topic"].as<std::string>(),
            .server_url_list = pub_topic_options_node["server_url_list"].as<std::vector<std::string>>()};

        rhs.pub_topics_options.emplace_back(std::move(pub_topic_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::net_extension {

void UdpTransportDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Udp transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void UdpTransportDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void UdpTransportDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (udp_svr_ptr_)
    udp_svr_ptr_->Shutdown();

  if (msg_handle_catalog_ptr_)
    msg_handle_catalog_ptr_->Shutdown();

  udp_cli_pool_ptr_->Shutdown();
}

bool UdpTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    namespace util = ibmw::common::util;

    const auto& info = send_type_wrapper.info;

    std::vector<std::string> server_url_list;

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
      server_url_list = find_option_itr->server_url_list;
    }

    std::vector<boost::asio::ip::udp::endpoint> server_ep_vec;
    for (const auto& publish_add : server_url_list) {
      auto v = ibmw::common::util::SplitToVec<std::string>(publish_add, ":");
      boost::asio::ip::udp::endpoint ep{
          boost::asio::ip::make_address(v[0].c_str()),
          static_cast<uint16_t>(atoi(v[1].c_str()))};

      server_ep_vec.emplace_back(ep);
    }

    pub_cfg_info_map_.emplace(
        info.topic_name,
        PubCfgInfo{
            .server_ep_vec = std::move(server_ep_vec)});

    // Check path
    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    MW_REQUIRE(pattern.size() <= 255, "Too long uri: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool UdpTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    if (!udp_svr_ptr_) {
      MW_ERROR("Failed to subscribe: please set udp listen IP and port in the configuration.");
      return false;
    }

    namespace util = ibmw::common::util;

    const auto& info = message_handler.info;

    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    auto find_itr = udp_message_handler_map_.find(pattern);
    if (find_itr != udp_message_handler_map_.end()) {
      find_itr->second->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    auto msg_handler_tool_unique_ptr = std::make_unique<ibmw::runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

    auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();

    udp_message_handler_map_.emplace(pattern, std::move(msg_handler_tool_unique_ptr));

    auto handle = [this, topic_name = info.topic_name, msg_handler_tool_ptr](
                      const std::shared_ptr<boost::asio::streambuf>& msg_buf_ptr) {
      auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

      // parse msg buf
      util::ConstBufferOperator buf_oper(
          static_cast<const char*>(msg_buf_ptr->data().data()),
          msg_buf_ptr->size());

      // pattern
      buf_oper.GetString(util::BufferLenType::kUInt8);

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

    MW_INFO("Register udp handle for transport, uri '{}'", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void UdpTransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    namespace util = ibmw::common::util;

    const auto& info = msg_wrapper.info;

    std::string_view to_addr;
    if (msg_wrapper.ctx_ref) {
      to_addr = msg_wrapper.ctx_ref.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_TO_ADDR);
    }
    auto to_addr_vec = util::SplitToVec<std::string>(to_addr, ";");
    std::vector<boost::asio::ip::udp::endpoint> to_addr_server_ep_vec;
    for (const auto& to_addr : to_addr_vec) {
      constexpr std::string_view udp_prefix = "udp://";
      if (!to_addr.starts_with(udp_prefix)) {
        continue;
      }
      if (auto url_op = util::ParseUrl<std::string>(to_addr.substr(udp_prefix.size()))) {  // remove "udp://" and parse url
        boost::asio::ip::udp::endpoint ep{
            boost::asio::ip::make_address(url_op->host.c_str()),
            static_cast<uint16_t>(std::stoi(url_op->service))};
        to_addr_server_ep_vec.emplace_back(ep);
      } else {
        MW_WARN("Can not parse to_addr: {} for topic: {}", to_addr, info.topic_name);
      }
    }

    const std::vector<boost::asio::ip::udp::endpoint>* server_ep_vec_to_use = &to_addr_server_ep_vec;

    // Find from pub_cfg_info_map_
    if (server_ep_vec_to_use->empty()) {
      auto find_itr = pub_cfg_info_map_.find(info.topic_name);
      MW_REQUIRE(find_itr != pub_cfg_info_map_.end(),
                              "Can not find pub cfg info for topic: {}", info.topic_name);
      MW_REQUIRE(!find_itr->second.server_ep_vec.empty(),
                              "Server url list is empty for topic: {}", info.topic_name);
      server_ep_vec_to_use = &find_itr->second.server_ep_vec;
    }

    // Determine the data serialization type, first look for ctx. If it is not configured in ctx, use the first supported serialization type.
    auto publish_type_support_ref = info.msg_type_support_ref;

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

    // context
    std::vector<std::pair<std::string, std::string>> meta_key_vals;
    if (msg_wrapper.ctx_ref) {
      auto meta_key_vals_array = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
      MW_REQUIRE(meta_key_vals_array.len / 2 <= 255,
                              "Too much context meta, require less than 255, but actually {}.", meta_key_vals_array.len / 2);
      meta_key_vals.reserve(meta_key_vals_array.len / 2);
      for (size_t ii = 0; ii < meta_key_vals_array.len; ii += 2) {
        meta_key_vals.emplace_back(
            std::string(ibmw::util::ToStdStringView(meta_key_vals_array.str_array[ii])),
            std::string(ibmw::util::ToStdStringView(meta_key_vals_array.str_array[ii + 1])));
      }
    }

    size_t context_meta_kv_size = 1;
    for (const auto& [key, val] : meta_key_vals) {
      context_meta_kv_size += 2 + key.size();
      context_meta_kv_size += 2 + val.size();
    }

    // Set url
    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    // Fill the content and copy it directly
    auto msg_buf_ptr = std::make_shared<boost::asio::streambuf>();

    const auto* buffer_array_data = buffer_array_view_ptr->Data();
    const size_t buffer_array_len = buffer_array_view_ptr->Size();
    size_t msg_size = buffer_array_view_ptr->BufferSize();

    uint32_t pkg_size = 1 + pattern.size() +
                        1 + serialization_type.size() +
                        context_meta_kv_size +
                        msg_size;

    auto buf = msg_buf_ptr->prepare(pkg_size);

    util::BufferOperator buf_oper(static_cast<char*>(buf.data()), buf.size());

    buf_oper.SetString(pattern, util::BufferLenType::kUInt8);
    buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

    buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals.size()));
    for (const auto& [key, val] : meta_key_vals) {
      buf_oper.SetString(key, util::BufferLenType::kUInt16);
      buf_oper.SetString(val, util::BufferLenType::kUInt16);
    }

    // data
    for (size_t ii = 0; ii < buffer_array_len; ++ii) {
      buf_oper.SetBuffer(
          static_cast<const char*>(buffer_array_data[ii].data),
          buffer_array_data[ii].len);
    }

    msg_buf_ptr->commit(pkg_size);

    for (const auto& server_ep : *server_ep_vec_to_use) {
      MW_TRACE("Send msg to {}", server_ep.address().to_string());
      boost::asio::co_spawn(
          *io_ptr_,
          [this, server_ep, msg_buf_ptr]() -> boost::asio::awaitable<void> {
            ibmw::common::net::AsioUdpClient::Options client_options{
                .svr_ep = server_ep};

            auto cli = co_await udp_cli_pool_ptr_->GetClient(client_options);
            if (!cli) [[unlikely]] {
              MW_WARN("Can not get udp client!");
              co_return;
            }

            cli->SendMsg(msg_buf_ptr);
          },
          boost::asio::detached);
    }

    return;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

}  // namespace ibmw::extensions::net_extension
