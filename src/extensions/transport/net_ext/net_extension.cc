// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "net_ext/net_extension.h"

#include "engine/engine.h"
#include "net_ext/global.h"
#include "net_ext/http/http_transport_driver.h"
#include "net_ext/http/http_rpc_driver.h"
#include "net_ext/tcp/tcp_transport_driver.h"
#include "net_ext/udp/udp_transport_driver.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::net_extension::NetExtension::Options> {
  using Options = ibmw::extensions::net_extension::NetExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["thread_num"] = rhs.thread_num;

    if (rhs.http_options) {
      node["http_options"] = Node();
      node["http_options"]["listen_ip"] = rhs.http_options->listen_ip;
      node["http_options"]["listen_port"] = rhs.http_options->listen_port;
    }

    if (rhs.tcp_options) {
      node["tcp_options"] = Node();
      node["tcp_options"]["listen_ip"] = rhs.tcp_options->listen_ip;
      node["tcp_options"]["listen_port"] = rhs.tcp_options->listen_port;
    }

    if (rhs.udp_options) {
      node["udp_options"] = Node();
      node["udp_options"]["listen_ip"] = rhs.udp_options->listen_ip;
      node["udp_options"]["listen_port"] = rhs.udp_options->listen_port;
      node["udp_options"]["max_pkg_size"] = rhs.udp_options->max_pkg_size;
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    MW_REQUIRE(node["thread_num"], "net_extension option thread_num is required.");
    rhs.thread_num = node["thread_num"].as<uint32_t>();

    if (node["http_options"]) {
      MW_REQUIRE(node["http_options"]["listen_port"], "net_extension option http_options.listen_port is required.");
      rhs.http_options = Options::HttpOptions{
          .listen_ip = node["http_options"]["listen_ip"].as<std::string>(Options::kDefaultListenIp),
          .listen_port = node["http_options"]["listen_port"].as<uint16_t>(),
      };
    }

    if (node["tcp_options"]) {
      MW_REQUIRE(node["tcp_options"]["listen_port"], "net_extension option tcp_options.listen_port is required.");
      rhs.tcp_options = Options::TcpOptions{
          .listen_ip = node["tcp_options"]["listen_ip"].as<std::string>(Options::kDefaultListenIp),
          .listen_port = node["tcp_options"]["listen_port"].as<uint16_t>(),
      };
    }

    if (node["udp_options"]) {
      MW_REQUIRE(node["udp_options"]["listen_port"], "net_extension option udp_options.listen_port is required.");
      rhs.udp_options = Options::UdpOptions{
          .listen_ip = node["udp_options"]["listen_ip"].as<std::string>(Options::kDefaultListenIp),
          .listen_port = node["udp_options"]["listen_port"].as<uint16_t>(),
      };
      if (node["udp_options"]["max_pkg_size"]) {
        rhs.udp_options->max_pkg_size = node["udp_options"]["max_pkg_size"].as<uint32_t>();
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::net_extension {

auto WrapIbmwLoggerRef(ibmw::logger::LoggerRef logger_ref)
    -> std::shared_ptr<ibmw::common::util::LoggerWrapper> {
  return std::make_shared<ibmw::common::util::LoggerWrapper>(
      ibmw::common::util::LoggerWrapper{
          .get_log_level_func = [logger_ref]() -> uint32_t {
            return logger_ref.GetLogLevel();
          },
          .log_func = [logger_ref](uint32_t lvl,
                                   uint32_t line,
                                   const char* file_name,
                                   const char* function_name,
                                   const char* log_data,
                                   size_t log_data_size) {
            logger_ref.Log(
                lvl, line, file_name, function_name, log_data, log_data_size);  //
          }});
}

// Resolves g_logger on every call: executor threads are joined after the
// logging subsystem stops, when g_logger has reverted to its fallback.
auto WrapExtensionLogger() -> std::shared_ptr<ibmw::common::util::LoggerWrapper> {
  return std::make_shared<ibmw::common::util::LoggerWrapper>(
      ibmw::common::util::LoggerWrapper{
          .get_log_level_func = []() -> uint32_t { return g_logger.Get().GetLogLevel(); },
          .log_func = [](uint32_t lvl,
                         uint32_t line,
                         const char* file_name,
                         const char* function_name,
                         const char* log_data,
                         size_t log_data_size) {
            g_logger.Get().Log(lvl, line, file_name, function_name, log_data, log_data_size);
          }});
}

NetExtension::NetExtension() {
  SetExtensionLogger(&g_logger);
}

NetExtension::Options NetExtension::ParseOptions(const YAML::Node& node) {
  return node.as<Options>();
}

bool NetExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = ParseOptions(extension_options_node);
  }

  asio_executor_ptr_ = std::make_shared<ibmw::common::net::AsioExecutor>(options_.thread_num);
  asio_executor_ptr_->SetLogger(WrapExtensionLogger());

  // http
  http_cli_pool_ptr_ = std::make_shared<ibmw::common::net::AsioHttpClientPool>(asio_executor_ptr_->IO());

  core_ptr_->RegisterHook(
      runtime::core::BeforePhase(runtime::core::Phase::kStart),
      [this] {
        http_cli_pool_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
        http_cli_pool_ptr_->Initialize(ibmw::common::net::AsioHttpClientPool::Options{});
        http_cli_pool_ptr_->Start();
      });

  core_ptr_->RegisterHook(
      runtime::core::AfterPhase(runtime::core::Phase::kStopping),
      [this] {
        http_cli_pool_ptr_->Shutdown();
      });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                  runtime::core::Subsystem::kService),
                          [this] { RegisterHttpRpcDriver(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                  runtime::core::Subsystem::kTransport),
                          [this] { RegisterHttpTransportDriver(); });

  if (options_.http_options) {
    http_svr_ptr_ = std::make_shared<ibmw::common::net::AsioHttpServer>(asio_executor_ptr_->IO());
    core_ptr_->RegisterHook(
        runtime::core::BeforePhase(runtime::core::Phase::kStart),
        [this] {
          http_svr_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
          http_svr_ptr_->Initialize(ibmw::common::net::AsioHttpServer::Options{
              .ep = {boost::asio::ip::make_address(options_.http_options->listen_ip),
                     options_.http_options->listen_port}});
          http_svr_ptr_->Start();
        });
    core_ptr_->RegisterHook(
        runtime::core::AfterPhase(runtime::core::Phase::kStopping),
        [this] {
          http_svr_ptr_->Shutdown();
        });
  }

  // tcp
  tcp_cli_pool_ptr_ = std::make_shared<ibmw::common::net::AsioTcpClientPool>(asio_executor_ptr_->IO());

  core_ptr_->RegisterHook(
      runtime::core::BeforePhase(runtime::core::Phase::kStart),
      [this] {
        tcp_cli_pool_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
        tcp_cli_pool_ptr_->Initialize(ibmw::common::net::AsioTcpClientPool::Options{});
        tcp_cli_pool_ptr_->Start();
      });

  core_ptr_->RegisterHook(
      runtime::core::AfterPhase(runtime::core::Phase::kStopping),
      [this] {
        tcp_cli_pool_ptr_->Shutdown();
      });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                  runtime::core::Subsystem::kTransport),
                          [this] { RegisterTcpTransportDriver(); });

  if (options_.tcp_options) {
    tcp_msg_handle_catalog_ptr_ = std::make_shared<MsgHandleCatalog<boost::asio::ip::tcp::endpoint>>();
    tcp_svr_ptr_ = std::make_shared<ibmw::common::net::AsioTcpServer>(asio_executor_ptr_->IO());

    core_ptr_->RegisterHook(
        runtime::core::BeforePhase(runtime::core::Phase::kStart),
        [this] {
          tcp_svr_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
          tcp_svr_ptr_->RegisterMsgHandle(tcp_msg_handle_catalog_ptr_->GetMsgHandleFunc());
          tcp_svr_ptr_->Initialize(ibmw::common::net::AsioTcpServer::Options{
              .ep = {boost::asio::ip::make_address(options_.tcp_options->listen_ip),
                     options_.tcp_options->listen_port}});
          tcp_svr_ptr_->Start();
        });

    core_ptr_->RegisterHook(
        runtime::core::AfterPhase(runtime::core::Phase::kStopping),
        [this] {
          tcp_svr_ptr_->Shutdown();
        });
  }

  // udp
  udp_cli_pool_ptr_ = std::make_shared<ibmw::common::net::AsioUdpClientPool>(asio_executor_ptr_->IO());

  core_ptr_->RegisterHook(
      runtime::core::BeforePhase(runtime::core::Phase::kStart),
      [this] {
        udp_cli_pool_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
        udp_cli_pool_ptr_->Initialize(ibmw::common::net::AsioUdpClientPool::Options{});
        udp_cli_pool_ptr_->Start();
      });

  core_ptr_->RegisterHook(
      runtime::core::AfterPhase(runtime::core::Phase::kStopping),
      [this] {
        udp_cli_pool_ptr_->Shutdown();
      });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                  runtime::core::Subsystem::kTransport),
                          [this] { RegisterUdpTransportDriver(); });

  if (options_.udp_options) {
    udp_msg_handle_catalog_ptr_ = std::make_shared<MsgHandleCatalog<boost::asio::ip::udp::endpoint>>();
    udp_svr_ptr_ = std::make_shared<ibmw::common::net::AsioUdpServer>(asio_executor_ptr_->IO());

    core_ptr_->RegisterHook(
        runtime::core::BeforePhase(runtime::core::Phase::kStart),
        [this] {
          udp_svr_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
          udp_svr_ptr_->RegisterMsgHandle(udp_msg_handle_catalog_ptr_->GetMsgHandleFunc());
          udp_svr_ptr_->Initialize(ibmw::common::net::AsioUdpServer::Options{
              .ep = {boost::asio::ip::make_address(options_.udp_options->listen_ip),
                     options_.udp_options->listen_port},
              .max_package_size = options_.udp_options->max_pkg_size});
          udp_svr_ptr_->Start();
        });

    core_ptr_->RegisterHook(
        runtime::core::AfterPhase(runtime::core::Phase::kStopping),
        [this] {
          udp_svr_ptr_->Shutdown();
        });
  }

  asio_executor_ptr_->Start();

  core_ptr_->RegisterHook(
      runtime::core::AfterPhase(runtime::core::Phase::kStopping),
      [this] {
        asio_executor_ptr_->Shutdown();
        asio_executor_ptr_->Join();
      });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void NetExtension::OnShutdown() {
  // udp
  if (udp_svr_ptr_)
    udp_svr_ptr_->Shutdown();

  if (udp_msg_handle_catalog_ptr_)
    udp_msg_handle_catalog_ptr_->Shutdown();

  if (udp_cli_pool_ptr_)
    udp_cli_pool_ptr_->Shutdown();

  // tcp
  if (tcp_svr_ptr_)
    tcp_svr_ptr_->Shutdown();

  if (tcp_msg_handle_catalog_ptr_)
    tcp_msg_handle_catalog_ptr_->Shutdown();

  if (tcp_cli_pool_ptr_)
    tcp_cli_pool_ptr_->Shutdown();

  // http
  if (http_svr_ptr_)
    http_svr_ptr_->Shutdown();

  if (http_cli_pool_ptr_)
    http_cli_pool_ptr_->Shutdown();

  // asio
  if (asio_executor_ptr_) {
    asio_executor_ptr_->Shutdown();
    asio_executor_ptr_->Join();
  }
}

void NetExtension::RegisterHttpRpcDriver() {
  std::unique_ptr<runtime::core::service::ServiceDriver> http_rpc_driver_ptr =
      std::make_unique<HttpRpcDriver>(asio_executor_ptr_->IO(),
                                       http_cli_pool_ptr_,
                                       http_svr_ptr_);

  core_ptr_->GetServiceDispatcher().RegisterRpcDriver(std::move(http_rpc_driver_ptr));
}

void NetExtension::RegisterHttpTransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> http_transport_driver_ptr =
      std::make_unique<HttpTransportDriver>(asio_executor_ptr_->IO(),
                                           http_cli_pool_ptr_,
                                           http_svr_ptr_);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(http_transport_driver_ptr), {},
      runtime::core::transport::SerializedTransportCapabilityV2{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kRuntimeTap,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .capture_priority = 8,
          .provider_name = Name()});
}

void NetExtension::RegisterTcpTransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> tcp_transport_driver_ptr =
      std::make_unique<TcpTransportDriver>(asio_executor_ptr_->IO(),
                                          tcp_cli_pool_ptr_,
                                          tcp_svr_ptr_,
                                          tcp_msg_handle_catalog_ptr_);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(tcp_transport_driver_ptr), {},
      runtime::core::transport::SerializedTransportCapabilityV2{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kRuntimeTap,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .capture_priority = 6,
          .provider_name = Name()});
}

void NetExtension::RegisterUdpTransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> udp_transport_driver_ptr =
      std::make_unique<UdpTransportDriver>(asio_executor_ptr_->IO(),
                                          udp_cli_pool_ptr_,
                                          udp_svr_ptr_,
                                          udp_msg_handle_catalog_ptr_);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(udp_transport_driver_ptr), {},
      runtime::core::transport::SerializedTransportCapabilityV2{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kRuntimeTap,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .capture_priority = 7,
          .provider_name = Name()});
}

}  // namespace ibmw::extensions::net_extension

IBMW_EXT_ENTRY(ibmw::extensions::net_extension::NetExtension)
