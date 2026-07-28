// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "net_ext/http/http_rpc_driver.h"

#include <regex>

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_status.h"
#include "cpp/util/buffer.h"
#include "cpp/util/type_support.h"
#include "engine/service/service_driver_tools.h"
#include "net_ext/global.h"
#include "base/url_encode.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::net_extension::HttpRpcDriver::Options> {
  using Options = ibmw::extensions::net_extension::HttpRpcDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["clients"] = YAML::Node();
    for (const auto& client_options : rhs.clients_options) {
      Node client_options_node;
      client_options_node["func_name"] = client_options.func_name;
      client_options_node["server_url"] = client_options.server_url;
      node["clients"].push_back(client_options_node);
    }

    node["servers"] = YAML::Node();
    for (const auto& server_options : rhs.servers_options) {
      Node server_options_node;
      server_options_node["func_name"] = server_options.func_name;
      node["servers"].push_back(server_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["clients"] && node["clients"].IsSequence()) {
      for (const auto& client_options_node : node["clients"]) {
        auto client_options = Options::ClientOptions{
            .func_name = client_options_node["func_name"].as<std::string>(),
            .server_url = client_options_node["server_url"].as<std::string>()};

        rhs.clients_options.emplace_back(std::move(client_options));
      }
    }

    if (node["servers"] && node["servers"].IsSequence()) {
      for (const auto& server_options_node : node["servers"]) {
        auto server_options = Options::ServerOptions{
            .func_name = server_options_node["func_name"].as<std::string>()};

        rhs.servers_options.emplace_back(std::move(server_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::net_extension {

void HttpRpcDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Http Service driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void HttpRpcDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void HttpRpcDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (http_svr_ptr_)
    http_svr_ptr_->Shutdown();
  http_cli_pool_ptr_->Shutdown();
}

bool HttpRpcDriver::RegisterServiceFunc(
    const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Service func can only be registered when state is 'Init'.");
      return false;
    }

    if (!http_svr_ptr_) {
      MW_ERROR("Failed to register service function: please set http listen IP and port in the configuration.");
      return false;
    }

    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    namespace util = ibmw::common::util;

    std::string pattern =
        std::string("/rpc") +
        std::string(rpc::GetFuncNameWithoutPrefix(service_func_wrapper.info.func_name));

    ibmw::common::net::AsioHttpServer::HttpHandle<http::dynamic_body> http_handle =
        [this, &service_func_wrapper](
            const http::request<http::dynamic_body>& req,
            http::response<http::dynamic_body>& rsp,
            std::chrono::nanoseconds timeout)
        -> asio::awaitable<ibmw::common::net::AsioHttpServer::HttpHandleStatus> {
      // Create a service invoke wrapper
      auto service_invoke_wrapper_ptr = std::make_shared<runtime::core::service::ServiceMessageFrame>(
          runtime::core::service::ServiceMessageFrame{.info = service_func_wrapper.info});
      const auto& info = service_invoke_wrapper_ptr->info;

      // Create service ctx
      auto ctx_ptr = std::make_shared<ibmw::rpc::Context>(ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT);
      service_invoke_wrapper_ptr->ctx_ref = ctx_ptr;

      // Serialization type
      std::string serialization_type;

      // set access_control_allow_origin (this is set all)
      rsp.set(http::field::access_control_allow_origin, "*");

      // if is a options request
      if (req.method() == http::verb::options) {
        rsp.set(http::field::access_control_allow_methods, "POST, OPTIONS");
        rsp.set(http::field::access_control_allow_headers, "Content-Type, Authorization, X-Requested-With");
        rsp.set(http::field::access_control_max_age, "86400");

        rsp.result(http::status::ok);
        rsp.keep_alive(req.keep_alive());
        rsp.prepare_payload();
        co_return ibmw::common::net::AsioHttpServer::HttpHandleStatus::kOk;
      }

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

      ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, serialization_type);

      // Read other fields from http header into context
      for (auto const& field : req) {
        ctx_ptr->SetMetaValue(
            ibmw::common::util::HttpHeaderDecode(
                ibmw::common::net::ToStdStringView(field.name_string())),
            ibmw::common::util::HttpHeaderDecode(
                ibmw::common::net::ToStdStringView(field.value())));
      }

      ctx_ptr->SetFunctionName(info.func_name);
      ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER, Name());

      ctx_ptr->SetTimeout(timeout);

      // service req deserialization
      const auto& req_beast_buf = req.body().data();
      std::vector<ibmw_buffer_view_t> buffer_view_vec;

      for (auto const buf : boost::beast::buffers_range_ref(req_beast_buf)) {
        buffer_view_vec.emplace_back(ibmw_buffer_view_t{
            .data = const_cast<void*>(buf.data()),
            .len = buf.size()});
      }

      ibmw_buffer_array_view_t buffer_array_view{
          .data = buffer_view_vec.data(),
          .len = buffer_view_vec.size()};

      std::shared_ptr<void> service_req_ptr = info.req_type_support_ref.CreateSharedPtr();
      service_invoke_wrapper_ptr->req_ptr = service_req_ptr.get();

      bool deserialize_ret = info.req_type_support_ref.Deserialize(
          serialization_type, buffer_array_view, service_req_ptr.get());

      MW_REQUIRE(deserialize_ret, "Http req deserialize failed.");

      // Create service rsp
      std::shared_ptr<void> service_rsp_ptr = info.rsp_type_support_ref.CreateSharedPtr();
      service_invoke_wrapper_ptr->rsp_ptr = service_rsp_ptr.get();

      // Set callback
      uint32_t ret_code = 0;
      auto sig_timer_ptr = std::make_shared<asio::steady_timer>(*io_ptr_, std::chrono::nanoseconds::max());

      service_invoke_wrapper_ptr->callback =
          [service_invoke_wrapper_ptr,
           ctx_ptr,
           &req,
           &rsp,
           service_req_ptr,
           service_rsp_ptr,
           serialization_type{std::move(serialization_type)},
           sig_timer_ptr,
           &ret_code](ibmw::rpc::Status status) {
            if (!status.OK()) [[unlikely]] {
              ret_code = status.Code();

            } else {
              // service rsp serialization
              auto buffer_array_view_ptr = ibmw::runtime::core::service::TrySerializeRspWithCache(*service_invoke_wrapper_ptr, serialization_type);

              if (!buffer_array_view_ptr) [[unlikely]] {
                ret_code = IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED;
              } else {
                // Fill http rsp package and copy it directly
                size_t rsp_size = buffer_array_view_ptr->BufferSize();
                auto rsp_beast_buf = rsp.body().prepare(rsp_size);

                const auto* data = buffer_array_view_ptr->Data();
                auto buffer_array_pos = 0;
                size_t buffer_pos = 0;

                for (auto buf : boost::beast::buffers_range_ref(rsp_beast_buf)) {
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
                rsp.body().commit(rsp_size);

                rsp.keep_alive(req.keep_alive());
                rsp.prepare_payload();
              }
            }
            sig_timer_ptr->expires_at(std::chrono::steady_clock::time_point::min());
          };

      // Call service rpc
      service_func_wrapper.service_func(service_invoke_wrapper_ptr);

      try {
        co_await sig_timer_ptr->async_wait(asio::use_awaitable);
      } catch (const boost::system::system_error& e) {
        if (e.code() != boost::asio::error::operation_aborted) [[unlikely]] {
          MW_ERROR("Http handle for service, async_wait get exception: {}", e.what());
          ret_code = ibmw_rpc_status_code_t::IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR;
        }
      }

      MW_REQUIRE(ret_code == 0, "Handle rpc failed, code: {}.", ret_code);

      co_return ibmw::common::net::AsioHttpServer::HttpHandleStatus::kOk;
    };

    http_svr_ptr_->RegisterHttpHandleFunc<http::dynamic_body>(
        pattern, std::move(http_handle));
    MW_INFO("Register http handle for service, uri '{}'", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool HttpRpcDriver::RegisterClientFunc(
    const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Client func can only be registered when state is 'Init'.");
      return false;
    }

    const auto& info = client_func_wrapper.info;

    auto find_client_option = std::find_if(
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

    if (find_client_option == options_.clients_options.end()) {
      MW_WARN("Server url is not set for func: {}", info.func_name);
    } else {
      client_server_url_map_.emplace(rpc::GetFuncNameWithoutPrefix(info.func_name), find_client_option->server_url);
    }

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void HttpRpcDriver::Call(
    const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept {
  try {
    if (state_.load() != State::kStart) [[unlikely]] {
      MW_WARN("Method can only be called when state is 'Start'.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    namespace util = ibmw::common::util;

    const auto& info = client_call_wrapper_ptr->info;

    auto real_func_name = rpc::GetFuncNameWithoutPrefix(info.func_name);

    // Check ctx, priority of to_addr: ctx > server_url
    auto to_addr = client_call_wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR);
    if (to_addr.empty()) {
      auto find_itr = client_server_url_map_.find(real_func_name);
      if (find_itr != client_server_url_map_.end()) {
        to_addr = find_itr->second;
      } else {
        MW_WARN("Server url is not set for func: {}", info.func_name);
        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_ADDR));
        return;
      }
    }

    auto url = util::ParseUrl<std::string_view>(to_addr);
    if (!url) {
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_ADDR));
      return;
    }

    asio::co_spawn(
        *io_ptr_,
        [http_cli_pool_ptr{http_cli_pool_ptr_},
         client_call_wrapper_ptr,
         url]() -> asio::awaitable<void> {
          const auto& info = client_call_wrapper_ptr->info;

          std::string url_path(url->path);
          if (url_path.empty()) {
            url_path = "/rpc" + std::string(rpc::GetFuncNameWithoutPrefix(info.func_name));
          }

          try {
            ibmw::common::net::AsioHttpClient::Options cli_options{
                .host = std::string(url->host),
                .service = std::string(url->service)};

            auto client_ptr = co_await http_cli_pool_ptr->GetClient(cli_options);
            if (!client_ptr) [[unlikely]] {
              MW_WARN("Can not get http client!");
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
              co_return;
            }

            http::request<http::dynamic_body> req{http::verb::post, url_path, 11};
            req.set(http::field::host,
                    ibmw::common::net::ToBeastStringView(url->host));
            req.set(http::field::user_agent, "ibmw");

            auto timeout = client_call_wrapper_ptr->ctx_ref.Timeout();
            if (timeout <= std::chrono::nanoseconds(0))
              timeout = std::chrono::seconds(5);
            req.set(http::field::timeout,
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(timeout).count()));

            std::string serialization_type(
                client_call_wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE));
            if (serialization_type == "json") {
              req.set(http::field::content_type, "application/json");
            } else if (serialization_type == "cdr") {
              req.set(http::field::content_type, "application/cdr");
            } else if (serialization_type == "ros2") {
              req.set(http::field::content_type, "application/ros2");
            } else {
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_SERIALIZATION_TYPE));
              co_return;
            }

            // Set other context meta fields to http header
            auto [meta_key_vals_array, meta_key_vals_array_len] = client_call_wrapper_ptr->ctx_ref.GetMetaKeyValsArray();
            for (size_t ii = 0; ii < meta_key_vals_array_len; ii += 2) {
              auto key = ibmw::util::ToStdStringView(meta_key_vals_array[ii]);
              auto val = ibmw::util::ToStdStringView(meta_key_vals_array[ii + 1]);
              req.set(
                  ibmw::common::util::HttpHeaderEncode(key),
                  ibmw::common::util::HttpHeaderEncode(val));
            }

            // client req serialization
            auto buffer_array_view_ptr = ibmw::runtime::core::service::TrySerializeReqWithCache(*client_call_wrapper_ptr, serialization_type);
            if (!buffer_array_view_ptr) [[unlikely]] {
              // Serialization failed
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED));
              co_return;
            }

            // Fill the http req package and copy it directly
            size_t req_size = buffer_array_view_ptr->BufferSize();
            auto req_beast_buf = req.body().prepare(req_size);

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
            req.body().commit(req_size);

            req.prepare_payload();
            req.keep_alive(true);

            auto rsp = co_await client_ptr->HttpSendRecvCo<http::dynamic_body, http::dynamic_body>(req, timeout);

            if (rsp.result() != http::status::ok) [[unlikely]] {
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_FOUND));
              co_return;
            }

            // client rsp deserialization
            const auto& rsp_beast_buf = rsp.body().data();
            std::vector<ibmw_buffer_view_t> buffer_view_vec;

            for (auto const buf : boost::beast::buffers_range_ref(rsp_beast_buf)) {
              buffer_view_vec.emplace_back(ibmw_buffer_view_t{
                  .data = buf.data(),
                  .len = buf.size()});
            }

            ibmw_buffer_array_view_t buffer_array_view{
                .data = buffer_view_vec.data(),
                .len = buffer_view_vec.size()};

            bool deserialize_ret = info.rsp_type_support_ref.Deserialize(
                serialization_type, buffer_array_view, client_call_wrapper_ptr->rsp_ptr);

            if (!deserialize_ret) {
              // Deserialization failed
              CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DESERIALIZATION_FAILED));
              co_return;
            }

            CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_OK));

            co_return;
          } catch (const std::exception& e) {
            MW_WARN("Http call get exception, info: {}", e.what());
          }

          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_UNKNOWN));
          co_return;
        },
        asio::detached);
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

}  // namespace ibmw::extensions::net_extension
