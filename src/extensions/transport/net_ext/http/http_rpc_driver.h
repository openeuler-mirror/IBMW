// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/service/service_driver.h"
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
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::service::ServiceCatalog* service_catalog_ptr_ = nullptr;

  std::unordered_map<std::string_view, std::string_view> client_server_url_map_;

  std::shared_ptr<boost::asio::io_context> io_ptr_;
  std::shared_ptr<ibmw::common::net::AsioHttpClientPool> http_cli_pool_ptr_;
  std::shared_ptr<ibmw::common::net::AsioHttpServer> http_svr_ptr_;
};

}  // namespace ibmw::extensions::net_extension
