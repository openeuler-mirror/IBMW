// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <optional>

#include "engine/extension/extension_base.h"
#include "net_base/asio_http_cli.h"
#include "net_base/asio_http_svr.h"
#include "net_base/asio_tcp_cli.h"
#include "net_base/asio_tcp_svr.h"
#include "net_base/asio_tools.h"
#include "net_base/asio_udp_cli.h"
#include "net_base/asio_udp_svr.h"
#include "net_ext/msg_handle_catalog.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::net_extension {

class NetExtension : public ExtensionBase {
 public:
  struct Options {
    uint32_t thread_num = 2;

    // Loopback unless configured: exposing a listener is an explicit choice.
    static constexpr const char* kDefaultListenIp = "127.0.0.1";

    struct HttpOptions {
      std::string listen_ip = kDefaultListenIp;
      uint16_t listen_port = 0;
    };
    std::optional<HttpOptions> http_options;

    struct TcpOptions {
      std::string listen_ip = kDefaultListenIp;
      uint16_t listen_port = 0;
    };
    std::optional<TcpOptions> tcp_options;

    struct UdpOptions {
      std::string listen_ip = kDefaultListenIp;
      uint16_t listen_port = 0;
      uint32_t max_pkg_size = 1024;
    };
    std::optional<UdpOptions> udp_options;
  };

 public:
  NetExtension();
  ~NetExtension() override = default;

  std::string_view Name() const noexcept override { return "net_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

  // Throws, naming the option, when a required one is missing.
  static Options ParseOptions(const YAML::Node& node);

 private:
  void RegisterHttpRpcDriver();
  void RegisterHttpTransportDriver();
  void RegisterTcpTransportDriver();
  void RegisterUdpTransportDriver();

 private:
  Options options_;

  std::shared_ptr<ibmw::common::net::AsioExecutor> asio_executor_ptr_;

  std::shared_ptr<ibmw::common::net::AsioHttpClientPool> http_cli_pool_ptr_;
  std::shared_ptr<ibmw::common::net::AsioHttpServer> http_svr_ptr_;

  std::shared_ptr<ibmw::common::net::AsioTcpClientPool> tcp_cli_pool_ptr_;
  std::shared_ptr<MsgHandleCatalog<boost::asio::ip::tcp::endpoint>> tcp_msg_handle_catalog_ptr_;
  std::shared_ptr<ibmw::common::net::AsioTcpServer> tcp_svr_ptr_;

  std::shared_ptr<ibmw::common::net::AsioUdpClientPool> udp_cli_pool_ptr_;
  std::shared_ptr<MsgHandleCatalog<boost::asio::ip::udp::endpoint>> udp_msg_handle_catalog_ptr_;
  std::shared_ptr<ibmw::common::net::AsioUdpServer> udp_svr_ptr_;
};

}  // namespace ibmw::extensions::net_extension
