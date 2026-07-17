// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "boost/asio/io_context.hpp"
#include "engine/service/service_driver.h"
#include "grpc_ext/client/client_pool.h"
#include "grpc_ext/server/server.h"

namespace ibmw::extensions::grpc_extension {

class GrpcRpcDriver : public runtime::core::service::ServiceDriver {
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
  explicit GrpcRpcDriver(
      const std::shared_ptr<boost::asio::io_context>& io_ptr,
      const std::shared_ptr<server::AsioHttp2Server>& http2_svr_ptr,
      const std::shared_ptr<client::AsioHttp2ClientPool>& http2_cli_pool_ptr)
      : io_ptr_(io_ptr),
        http2_svr_ptr_(http2_svr_ptr),
        http2_cli_pool_ptr_(http2_cli_pool_ptr) {}

  ~GrpcRpcDriver() override = default;

  [[nodiscard]] std::string_view Name() const noexcept override { return "grpc"; }

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

  // client
  std::unordered_map<std::string, std::string> client_server_url_map_;

  // server
  std::shared_ptr<boost::asio::io_context> io_ptr_;
  std::shared_ptr<server::AsioHttp2Server> http2_svr_ptr_;
  std::shared_ptr<client::AsioHttp2ClientPool> http2_cli_pool_ptr_;
};

}  // namespace ibmw::extensions::grpc_extension
