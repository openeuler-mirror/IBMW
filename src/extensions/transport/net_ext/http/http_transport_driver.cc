// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "net_ext/http/http_transport_driver.h"
#include "c/transport/transport_context_base.h"
#include "cpp/util/type_support.h"
#include "net_ext/global.h"
#include "base/url_encode.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::net_extension::HttpTransportDriver::Options> {
  using Options = ibmw::extensions::net_extension::HttpTransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["publishers"] = YAML::Node();
    for (const auto& pub_topic_options : rhs.pub_topics_options) {
      Node pub_topic_options_node;
      pub_topic_options_node["topic"] = pub_topic_options.topic_name;
      pub_topic_options_node["server_url_list"] = pub_topic_options.server_url_list;
      node["publishers"].push_back(pub_topic_options_node);
    }

    node["subscribers"] = YAML::Node();
    for (const auto& sub_topic_options : rhs.sub_topics_options) {
      Node sub_topic_options_node;
      sub_topic_options_node["topic"] = sub_topic_options.topic_name;
      node["subscribers"].push_back(sub_topic_options_node);
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

    if (node["subscribers"] && node["subscribers"].IsSequence()) {
      for (const auto& sub_topic_options_node : node["subscribers"]) {
        auto sub_topic_options = Options::SubTopicOptions{
            .topic_name = sub_topic_options_node["topic"].as<std::string>()};

        rhs.sub_topics_options.emplace_back(std::move(sub_topic_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::net_extension {

void HttpTransportDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Http transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void HttpTransportDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void HttpTransportDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (http_svr_ptr_)
    http_svr_ptr_->Shutdown();

  http_cli_pool_ptr_->Shutdown();
}

bool HttpTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

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

    std::vector<ibmw::common::util::Url<std::string>> server_url_st_vec;
    for (const auto& publish_add : server_url_list) {
      auto url_op = util::ParseUrl<std::string>(publish_add);
      if (url_op) {
        server_url_st_vec.emplace_back(*url_op);
      } else {
        MW_WARN("Can not parse url: {}, topic: {}", publish_add, info.topic_name);
      }
    }

    pub_cfg_info_map_.emplace(
        info.topic_name,
        PubCfgInfo{
            .server_url_st_vec = std::move(server_url_st_vec)});

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool HttpTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    if (!http_svr_ptr_) {
      MW_ERROR("Failed to subscribe: please set http listen IP and port in the configuration.");
      return false;
    }

    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    namespace util = ibmw::common::util;

    const auto& info = message_handler.info;

    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    auto find_itr = http_message_handler_map_.find(pattern);
    if (find_itr != http_message_handler_map_.end()) {
      find_itr->second->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    auto msg_handler_tool_unique_ptr = std::make_unique<ibmw::runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

    auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();

    http_message_handler_map_.emplace(pattern, std::move(msg_handler_tool_unique_ptr));

    ibmw::common::net::AsioHttpServer::HttpHandle<http::dynamic_body> http_handle =
        [this, topic_name = info.topic_name, msg_handler_tool_ptr](
            const http::request<http::dynamic_body>& req,
            http::response<http::dynamic_body>& rsp,
            std::chrono::nanoseconds timeout)
        -> asio::awaitable<ibmw::common::net::AsioHttpServer::HttpHandleStatus> {
      // Get the serialization type
      std::string serialization_type;
      auto req_content_type_itr = req.find(http::field::content_type);
      MW_REQUIRE(req_content_type_itr != req.end(),
                              "Http req has no content type.");

      auto req_content_type_boost_sw = req_content_type_itr->value();
      std::string_view req_content_type(req_content_type_boost_sw.data(), req_content_type_boost_sw.size());
      if (req_content_type == "application/json" ||
          req_content_type == "application/json charset=utf-8") {
        serialization_type = "json";
        rsp.set(http::field::content_type, "application/json");
      } else if (req_content_type == "application/cdr") {
        serialization_type = "cdr";
        rsp.set(http::field::content_type, "application/cdr");
      } else if (req_content_type == "application/ros2") {
        serialization_type = "ros2";
        rsp.set(http::field::content_type, "application/ros2");
      } else {
        MW_THROW("Http req has invalid content type {}.", req_content_type);
      }

      rsp.keep_alive(req.keep_alive());
      rsp.prepare_payload();

      // context
      auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

      ctx_ptr->SetSerializationType(serialization_type);

      // Read other fields from http header into context
      for (auto const& field : req) {
        ctx_ptr->SetMetaValue(
            ibmw::common::util::HttpHeaderDecode(field.name_string()),
            ibmw::common::util::HttpHeaderDecode(field.value()));
      }

      ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, Name());

      // Get message buf
      const auto& req_beast_buf = req.body().data();
      std::vector<ibmw_buffer_view_t> buffer_view_vec;

      for (auto const buf : boost::beast::buffers_range_ref(req_beast_buf)) {
        buffer_view_vec.emplace_back(ibmw_buffer_view_t{
            .data = const_cast<void*>(buf.data()),
            .len = buf.size()});
      }

      ibmw::util::BufferArrayView buffer_array_view(buffer_view_vec);

      msg_handler_tool_ptr->DoMessageCallback(ctx_ptr, serialization_type, buffer_array_view);

      co_return ibmw::common::net::AsioHttpServer::HttpHandleStatus::kOk;
    };

    http_svr_ptr_->RegisterHttpHandleFunc<http::dynamic_body>(
        pattern, std::move(http_handle));

    MW_INFO("Register http handle for transport, uri '{}'", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void HttpTransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    namespace util = ibmw::common::util;

    const auto& info = msg_wrapper.info;

    // Parse from to_addr
    std::string_view to_addr;
    if (msg_wrapper.ctx_ref) {
      to_addr = msg_wrapper.ctx_ref.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_TO_ADDR);
    }
    auto to_addr_vec = util::SplitToVec<std::string>(to_addr, ";");
    std::vector<util::Url<std::string>> to_addr_server_url_vec;
    for (const auto& to_addr : to_addr_vec) {
      constexpr std::string_view http_prefix = "http://";
      if (!to_addr.starts_with(http_prefix)) {
        continue;
      }
      if (auto url_op = util::ParseUrl<std::string>(to_addr.substr(http_prefix.size()))) {  // remove "http://" and parse url
        to_addr_server_url_vec.emplace_back(*url_op);
      } else {
        MW_WARN("Can not parse to_addr: {} for topic: {}", to_addr, info.topic_name);
      }
    }

    const std::vector<util::Url<std::string>>* urls_to_use = &to_addr_server_url_vec;

    // Find from pub_cfg_info_map_
    if (urls_to_use->empty()) {
      auto find_itr = pub_cfg_info_map_.find(info.topic_name);
      MW_REQUIRE(find_itr != pub_cfg_info_map_.end(),
                              "Can not find pub cfg info for topic: {}", info.topic_name);
      MW_REQUIRE(!find_itr->second.server_url_st_vec.empty(),
                              "Server url list is empty for topic: {}", info.topic_name);
      urls_to_use = &find_itr->second.server_url_st_vec;
    }

    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    // http req
    auto req_ptr = std::make_shared<http::request<http::dynamic_body>>(
        http::verb::post, pattern, 11);
    req_ptr->set(http::field::user_agent, "ibmw");

    // Determine the data serialization type, first look for ctx. If it is not configured in ctx, use the first supported serialization type.
    auto publish_type_support_ref = info.msg_type_support_ref;

    std::string_view serialization_type;
    if (msg_wrapper.ctx_ref) {
      serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    }
    if (serialization_type.empty()) {
      serialization_type = publish_type_support_ref.DefaultSerializationType();
    }

    if (serialization_type == "json") {
      req_ptr->set(http::field::content_type, "application/json");
    } else if (serialization_type == "cdr") {
      req_ptr->set(http::field::content_type, "application/cdr");
    } else if (serialization_type == "ros2") {
      req_ptr->set(http::field::content_type, "application/ros2");
    } else {
      req_ptr->set(http::field::content_type, "application/" + std::string(serialization_type));
    }

    // Set other context meta fields to http header
    if (msg_wrapper.ctx_ref) {
      auto meta_key_vals_array = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
      for (size_t ii = 0; ii < meta_key_vals_array.len; ii += 2) {
        auto key = ibmw::util::ToStdStringView(meta_key_vals_array.str_array[ii]);
        auto val = ibmw::util::ToStdStringView(meta_key_vals_array.str_array[ii + 1]);
        req_ptr->set(
            ibmw::common::util::HttpHeaderEncode(key),
            ibmw::common::util::HttpHeaderEncode(val));
      }
    }

    // msg serialization
    auto buffer_array_view_ptr = ibmw::runtime::core::transport::SerializeMsgWithCache(msg_wrapper, serialization_type);
    MW_REQUIRE(
        buffer_array_view_ptr,
        "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, topic_name: {}, msg_type: {}",
        serialization_type, info.pkg_path, info.module_name, info.topic_name, info.msg_type);

    // Fill the http req package and copy it directly
    size_t msg_size = buffer_array_view_ptr->BufferSize();
    auto req_beast_buf = req_ptr->body().prepare(msg_size);

    const auto* data = buffer_array_view_ptr->Data();
    auto buffer_array_pos = 0;
    size_t buffer_pos = 0;

    for (auto buf : boost::beast::buffers_range_ref(req_beast_buf)) {
      size_t cur_beast_buf_pos = 0;
      while (cur_beast_buf_pos < buf.size()) {
        size_t cur_beast_buffer_size = buf.size() - cur_beast_buf_pos;
        size_t cur_buffer_size = data[buffer_array_pos].len - buffer_pos;

        size_t cur_copy_size = std::min(cur_beast_buffer_size, cur_buffer_size);

        memcpy(static_cast<char*>(buf.data()) + cur_beast_buf_pos,
               static_cast<const char*>(data[buffer_array_pos].data) + buffer_pos,
               cur_copy_size);

        buffer_pos += cur_copy_size;
        if (buffer_pos == data[buffer_array_pos].len) {
          ++buffer_array_pos;
          buffer_pos = 0;
        }

        cur_beast_buf_pos += cur_copy_size;
      }
    }
    req_ptr->body().commit(msg_size);

    req_ptr->keep_alive(true);

    for (const auto& server_url : *urls_to_use) {
      MW_TRACE("Publish to server_url: {}", server_url.host + ":" + server_url.service);
      asio::co_spawn(
          *io_ptr_,
          [this, server_url, req_ptr]() -> asio::awaitable<void> {
            ibmw::common::net::AsioHttpClient::Options cli_options{
                .host = server_url.host,
                .service = server_url.service};

            auto client_ptr = co_await http_cli_pool_ptr_->GetClient(cli_options);
            if (!client_ptr) [[unlikely]] {
              MW_WARN("Can not get http client!");
              co_return;
            }

            // todo:
            // To solve thread safety issues when setting the host in requests for multiple addresses: use value copies for all except the last one, which uses a direct pointer
            // Host and other header fields should be configured dynamically, not hardcoded.
            req_ptr->set(http::field::host, server_url.host);
            req_ptr->prepare_payload();

            auto rsp = co_await client_ptr->HttpSendRecvCo<http::dynamic_body, http::dynamic_body>(*req_ptr);

            if (rsp.result() != http::status::ok) {
              MW_WARN("http transport send get error: {} {}",
                         rsp.result_int(),
                         std::string(rsp.reason().data(), rsp.reason().size()));
            }
          },
          asio::detached);
    }

    return;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

}  // namespace ibmw::extensions::net_extension
