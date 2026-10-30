// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "engine/extension/extension_base.h"
#include "grpc_ext/client/client_pool.h"
#include "grpc_ext/server/server.h"
#include "net_base/asio_tools.h"

namespace ibmw::extensions::grpc_extension {

class GrpcExtension : public ExtensionBase {
 public:
  struct Options {
    uint32_t thread_num = 2;
    std::string listen_ip = "";
    uint16_t listen_port = 0;
    // Largest accepted request body (bytes) of the server.
    uint32_t max_request_body_size = 16U << 20;
  };

 public:
  GrpcExtension();
  ~GrpcExtension() override = default;

  [[nodiscard]] std::string_view Name() const noexcept override { return "grpc_extension"; }

  // The server has neither TLS nor authentication (EXT-001): whether a
  // listen address is reachable from this host only. Throws on an invalid
  // address.
  static bool IsLoopbackListenAddress(const std::string& listen_ip) {
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address_v4(listen_ip, ec);
    if (ec) throw std::invalid_argument("invalid grpc listen_ip '" + listen_ip + "': " + ec.message());
    return address.is_loopback();
  }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterGrpcRpcDriver();

 private:
  Options options_;

  std::shared_ptr<ibmw::common::net::AsioExecutor> asio_executor_ptr_;

  std::shared_ptr<extensions::grpc_extension::server::AsioHttp2Server> http2_svr_ptr_;
  std::shared_ptr<extensions::grpc_extension::client::AsioHttp2ClientPool> http2_cli_pool_ptr_;
};

}  // namespace ibmw::extensions::grpc_extension
