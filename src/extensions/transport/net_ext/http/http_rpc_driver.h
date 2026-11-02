// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string_view>

#include "engine/service/service_driver.h"
#include "engine/discovery/service_discovery_protocol.h"
#include "net_base/asio_http_cli.h"
#include "net_base/asio_http_svr.h"

namespace ibmw::extensions::net_extension {

class HttpRpcDriver : public runtime::core::service::ServiceDriver {
 public:
  struct Options {
    struct ClientOptions {
      std::string func_name;
      std::string server_url;
    };
    std::vector<ClientOptions> clients_options;

    struct ServerOptions {
      std::string func_name;
    };
    std::vector<ServerOptions> servers_options;

    // Origins allowed to call the services from a browser; "*" allows any.
    // Empty (default): no CORS headers are sent.
    std::vector<std::string> cors_allow_origins;
  };

 public:
  HttpRpcDriver(
      const std::shared_ptr<boost::asio::io_context>& io_ptr,
      const std::shared_ptr<ibmw::common::net::AsioHttpClientPool>& http_cli_pool_ptr,
      const std::shared_ptr<ibmw::common::net::AsioHttpServer>& http_svr_ptr)
      : io_ptr_(io_ptr),
        http_cli_pool_ptr_(http_cli_pool_ptr),
        http_svr_ptr_(http_svr_ptr) {}

  ~HttpRpcDriver() override = default;

  // A failed call is answered with HTTP 500 and its RPC status in this header;
  // clients that do not know the header still see the 500.
  static constexpr std::string_view kRpcStatusHeader = "ibmw-rpc_status";

  // RPC status for a non-200 answer: the status header if present, else
  // derived from the HTTP status code.
  static uint32_t RpcStatusFromHttp(unsigned http_status, std::string_view rpc_status_header) noexcept;

  std::string_view Name() const noexcept override { return "http"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void SetServiceCatalog(const runtime::core::service::ServiceCatalog* service_catalog_ptr) noexcept override {
    service_catalog_ptr_ = service_catalog_ptr;
  }

  bool RegisterServiceFunc(
      const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept override;
  bool RegisterClientFunc(
      const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept override;
  void Call(
      const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept override;

 private:
  // Sets Access-Control-Allow-Origin if the request origin is allowed.
  bool SetCorsAllowOrigin(
      const boost::beast::http::request<boost::beast::http::dynamic_body>& req,
      boost::beast::http::response<boost::beast::http::dynamic_body>& rsp) const;

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  Options options_;
  std::shared_ptr<ibmw::discovery::service_protocol::Catalog> discovery_catalog_ =
      std::make_shared<ibmw::discovery::service_protocol::Catalog>();
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::service::ServiceCatalog* service_catalog_ptr_ = nullptr;

  std::unordered_map<std::string_view, std::string_view> client_server_url_map_;

  std::shared_ptr<boost::asio::io_context> io_ptr_;
  std::shared_ptr<ibmw::common::net::AsioHttpClientPool> http_cli_pool_ptr_;
  std::shared_ptr<ibmw::common::net::AsioHttpServer> http_svr_ptr_;
};

}  // namespace ibmw::extensions::net_extension
