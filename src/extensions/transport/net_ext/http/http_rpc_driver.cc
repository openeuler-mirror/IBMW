// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "net_ext/http/http_rpc_driver.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <regex>

#include <boost/asio/any_completion_handler.hpp>

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_status.h"
#include "cpp/util/buffer.h"
#include "cpp/util/type_support.h"
#include "engine/service/service_driver_tools.h"
#include "net_ext/global.h"
#include "base/url_encode.h"
#include "net_base/http_media_type.h"
#include "net_ext/meta_filter.h"

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

    for (const auto& origin : rhs.cors_allow_origins)
      node["cors_allow_origins"].push_back(origin);

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

    if (const auto origins = node["cors_allow_origins"]; origins && origins.IsSequence()) {
      for (const auto& origin_node : origins)
        rhs.cors_allow_origins.emplace_back(origin_node.as<std::string>());
    } else if (origins && origins.IsScalar()) {  // a single origin (or "*")
      rhs.cors_allow_origins.emplace_back(origins.as<std::string>());
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
  if (http_svr_ptr_) {
    namespace http = boost::beast::http;
    using Server = ibmw::common::net::AsioHttpServer;
    http_svr_ptr_->RegisterHttpHandleFunc<http::string_body>(
        ibmw::discovery::service_protocol::kHttpPath,
        [catalog = discovery_catalog_](const http::request<http::dynamic_body>& req,
            http::response<http::string_body>& rsp, std::chrono::nanoseconds)
            -> boost::asio::awaitable<Server::HttpHandleStatus> {
          rsp.result(req.method() == http::verb::get ? http::status::ok : http::status::method_not_allowed);
          rsp.set(http::field::content_type, "application/json");
          rsp.set(http::field::cache_control, "no-store");
          rsp.body() = req.method() == http::verb::get ? catalog->Encode() : "{}";
          rsp.keep_alive(req.keep_alive()); rsp.prepare_payload();
          co_return Server::HttpHandleStatus::kOk;
        });
  }
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

bool HttpRpcDriver::SetCorsAllowOrigin(
    const boost::beast::http::request<boost::beast::http::dynamic_body>& req,
    boost::beast::http::response<boost::beast::http::dynamic_body>& rsp) const {
  namespace http = boost::beast::http;
  if (options_.cors_allow_origins.empty()) return false;

  const bool any_origin =
      std::find(options_.cors_allow_origins.begin(), options_.cors_allow_origins.end(), "*") !=
      options_.cors_allow_origins.end();
  // The answer depends on the origin, also when it is rejected or missing (caches).
  if (!any_origin) rsp.set(http::field::vary, "Origin");

  auto origin_itr = req.find(http::field::origin);
  if (origin_itr == req.end()) return false;
  std::string_view origin(origin_itr->value().data(), origin_itr->value().size());

  if (any_origin) {
    rsp.set(http::field::access_control_allow_origin, "*");
    return true;
  }
  for (const auto& allowed : options_.cors_allow_origins) {
    if (allowed == origin) {
      rsp.set(http::field::access_control_allow_origin, origin_itr->value());
      return true;
    }
  }
  return false;
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

      // CORS only for configured origins
      const bool cors_allowed = SetCorsAllowOrigin(req, rsp);

      // if is a options request
      if (req.method() == http::verb::options) {
        if (cors_allowed) {
          rsp.set(http::field::access_control_allow_methods, "POST, OPTIONS");
          rsp.set(http::field::access_control_allow_headers, "Content-Type, Authorization, X-Requested-With");
          rsp.set(http::field::access_control_max_age, "86400");
        }

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
      if (ibmw::common::net::HttpMediaTypeIs(req_content_type, "application/json")) {
        serialization_type = "json";
        rsp.set(http::field::content_type, "application/json");
      } else if (ibmw::common::net::HttpMediaTypeIs(req_content_type, "application/cdr")) {
        serialization_type = "cdr";
        rsp.set(http::field::content_type, "application/cdr");
      } else if (ibmw::common::net::HttpMediaTypeIs(req_content_type, "application/ros2")) {
        serialization_type = "ros2";
        rsp.set(http::field::content_type, "application/ros2");
      } else {
        MW_THROW("Http req has invalid content type {}.", req_content_type);
      }

      // Read other fields from http header into context; framework fields are
      // set afterwards so a peer cannot override them.
      for (auto const& field : req) {
        auto key = ibmw::common::util::HttpHeaderDecode(
            ibmw::common::net::ToStdStringView(field.name_string()));
        if (!IsPeerMetaKeyAllowed(key)) continue;
        ctx_ptr->SetMetaValue(
            key,
            ibmw::common::util::HttpHeaderDecode(
                ibmw::common::net::ToStdStringView(field.value())));
      }

      ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, serialization_type);
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

      // The callback may run on any thread, before or after this coroutine
      // suspends, and after it gave up. All hand-over goes through CallState
      // under its mutex: whoever finishes the call first (answer or timeout)
      // takes the suspended coroutine's handler and resumes it. Nothing but
      // this coroutine touches the timer.
      struct CallState {
        std::mutex mutex;
        std::atomic<bool> finished{false};  // written under mutex; also read as a hint
        uint32_t ret_code = 0;
        std::shared_ptr<ibmw::util::BufferArrayView> rsp_buf;
        asio::any_completion_handler<void()> waiter;
      };
      auto call_state = std::make_shared<CallState>();

      const auto deadline =
          timeout >= std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now()
              ? std::chrono::steady_clock::time_point::max()
              : std::chrono::steady_clock::now() + timeout;

      // Resumes the coroutine on its io_context. If that is already gone the
      // coroutine frame is leaked: destroying it here would touch dead services.
      auto resume = [](const std::weak_ptr<asio::io_context>& io_weak, asio::any_completion_handler<void()> waiter) {
        if (auto io = io_weak.lock()) {
          asio::post(io->get_executor(), std::move(waiter));
        } else {
          new asio::any_completion_handler<void()>(std::move(waiter));
        }
      };

      service_invoke_wrapper_ptr->callback =
          [service_invoke_wrapper_ptr,
           ctx_ptr,
           service_req_ptr,
           service_rsp_ptr,
           serialization_type{std::move(serialization_type)},
           io_weak = std::weak_ptr<asio::io_context>(io_ptr_),
           resume,
           call_state](ibmw::rpc::Status status) {
            // Abandoned (timed out): skip the serialization.
            if (call_state->finished.load(std::memory_order_relaxed)) return;

            uint32_t ret_code = 0;
            std::shared_ptr<ibmw::util::BufferArrayView> rsp_buf;
            if (!status.OK()) [[unlikely]] {
              ret_code = status.Code();
            } else {
              // service rsp serialization
              rsp_buf = ibmw::runtime::core::service::TrySerializeRspWithCache(*service_invoke_wrapper_ptr, serialization_type);
              if (!rsp_buf) [[unlikely]]
                ret_code = IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED;
            }

            asio::any_completion_handler<void()> waiter;
            {
              std::lock_guard lock(call_state->mutex);
              if (call_state->finished.load(std::memory_order_relaxed)) return;
              call_state->finished.store(true, std::memory_order_relaxed);
              call_state->ret_code = ret_code;
              call_state->rsp_buf = std::move(rsp_buf);
              waiter = std::move(call_state->waiter);
            }
            if (waiter) resume(io_weak, std::move(waiter));
          };

      // Call service rpc
      service_func_wrapper.service_func(service_invoke_wrapper_ptr);

      // A synchronous answer (the common case) needs no wait at all.
      if (!call_state->finished.load(std::memory_order_acquire)) {
        std::optional<asio::steady_timer> timer;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
          timer.emplace(*io_ptr_, deadline);
          timer->async_wait([call_state, io_weak = std::weak_ptr<asio::io_context>(io_ptr_), resume](
                                const boost::system::error_code& ec) {
            if (ec) return;  // cancelled: the call finished first
            asio::any_completion_handler<void()> waiter;
            {
              std::lock_guard lock(call_state->mutex);
              if (call_state->finished.load(std::memory_order_relaxed)) return;
              call_state->finished.store(true, std::memory_order_relaxed);
              call_state->ret_code = IBMW_RPC_STATUS_TIMEOUT;
              waiter = std::move(call_state->waiter);
            }
            if (waiter) resume(io_weak, std::move(waiter));
          });
        }

        // The initiation runs once this coroutine is suspended, so the handler
        // can be resumed from another thread as soon as it is stored.
        co_await asio::async_initiate<decltype(asio::use_awaitable), void()>(
            [&call_state, ex = io_ptr_->get_executor()](auto handler) {
              asio::any_completion_handler<void()> waiter(std::move(handler));
              {
                std::lock_guard lock(call_state->mutex);
                if (!call_state->finished.load(std::memory_order_relaxed)) {
                  call_state->waiter = std::move(waiter);
                  return;
                }
              }
              // Finished between the check above and now.
              asio::post(ex, std::move(waiter));
            },
            asio::use_awaitable);

        if (timer) timer->cancel();
      }

      uint32_t ret_code = 0;
      std::shared_ptr<ibmw::util::BufferArrayView> rsp_buf;
      {
        std::lock_guard lock(call_state->mutex);
        ret_code = call_state->ret_code;
        rsp_buf = std::move(call_state->rsp_buf);
      }

      MW_REQUIRE(ret_code == 0, "Handle rpc failed, code: {}.", ret_code);

      // Fill http rsp package and copy it directly
      size_t rsp_size = rsp_buf->BufferSize();
      auto rsp_beast_buf = rsp.body().prepare(rsp_size);

      const auto* data = rsp_buf->Data();
      size_t buffer_array_pos = 0;
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

      co_return ibmw::common::net::AsioHttpServer::HttpHandleStatus::kOk;
    };

    http_svr_ptr_->RegisterHttpHandleFunc<http::dynamic_body>(
        pattern, std::move(http_handle));
    MW_INFO("Register http handle for service, uri '{}'", pattern);

    const auto& info = service_func_wrapper.info;
    discovery_catalog_->Add(info.func_name, info.req_type_support_ref.TypeName(), info.rsp_type_support_ref.TypeName());

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

    // The views point into the call's context meta or into options_, both of
    // which outlive the call (the coroutine reads ctx_ref as well).
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
            // Rounded up: the header has whole seconds and the server must not
            // give up before the client does.
            req.set(http::field::timeout,
                    std::to_string(std::chrono::ceil<std::chrono::seconds>(timeout).count()));

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
            for (size_t ii = 0; ii + 1 < meta_key_vals_array_len; ii += 2) {
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
