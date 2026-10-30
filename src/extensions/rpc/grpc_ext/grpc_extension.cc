// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "grpc_ext/grpc_extension.h"

#include <memory>

#include "boost/asio/ip/address_v4.hpp"
#include "engine/engine.h"
#include "engine/service/service_driver.h"
#include "grpc_ext/global.h"
#include "grpc_ext/grpc_rpc_driver.h"
#include "grpc_ext/server/server.h"
#include "net_base/asio_tools.h"
#include "base/log_util.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::grpc_extension::GrpcExtension::Options> {
  using Options = ibmw::extensions::grpc_extension::GrpcExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["thread_num"] = rhs.thread_num;
    if (!rhs.listen_ip.empty() && rhs.listen_port != 0) {
      node["listen_ip"] = rhs.listen_ip;
      node["listen_port"] = rhs.listen_port;
      node["max_request_body_size"] = rhs.max_request_body_size;
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["thread_num"])
      rhs.thread_num = node["thread_num"].as<uint32_t>();

    if (node["listen_ip"])
      rhs.listen_ip = node["listen_ip"].as<std::string>();

    if (node["listen_port"])
      rhs.listen_port = node["listen_port"].as<uint16_t>();

    if (node["max_request_body_size"])
      rhs.max_request_body_size = node["max_request_body_size"].as<uint32_t>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::grpc_extension {

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

GrpcExtension::GrpcExtension() {
  SetExtensionLogger(&g_logger);
}

bool GrpcExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }
  // A gRPC message has a 5-byte prefix: a smaller limit rejects every request.
  MW_REQUIRE(options_.max_request_body_size >= 5, "Grpc max_request_body_size must be at least 5, got {}.",
             options_.max_request_body_size);

  asio_executor_ptr_ = std::make_shared<ibmw::common::net::AsioExecutor>(options_.thread_num);

  http2_cli_pool_ptr_ = std::make_shared<client::AsioHttp2ClientPool>(asio_executor_ptr_->IO());

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kService),
                          [this] { RegisterGrpcRpcDriver(); });

  core_ptr_->RegisterHook(runtime::core::BeforePhase(runtime::core::Phase::kStart), [this] {
    http2_cli_pool_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
    http2_cli_pool_ptr_->Initialize(client::ClientPoolOptions{.max_client_num = 100});
    http2_cli_pool_ptr_->Start();
  });

  core_ptr_->RegisterHook(runtime::core::AfterPhase(runtime::core::Phase::kStopping), [this] {
    http2_cli_pool_ptr_->Shutdown();
  });

  if (!options_.listen_ip.empty() && options_.listen_port != 0) {
    // Checked here so that a bad address fails initialization, not start.
    bool loopback = false;
    try {
      loopback = IsLoopbackListenAddress(options_.listen_ip);
    } catch (const std::invalid_argument& e) {
      MW_THROW("{}", e.what());
    }
    MW_CHECK_WARN(loopback,
                  "Grpc server listens on {}:{} in plaintext without authentication: every host that can reach it "
                  "can call all services. Listen on 127.0.0.1 or put it behind a TLS proxy.",
                  options_.listen_ip, options_.listen_port);

    http2_svr_ptr_ = std::make_shared<server::AsioHttp2Server>(asio_executor_ptr_->IO());
    core_ptr_->RegisterHook(runtime::core::BeforePhase(runtime::core::Phase::kStart), [this] {
      http2_svr_ptr_->SetLogger(WrapIbmwLoggerRef(g_logger.Get()));
      http2_svr_ptr_->Initialize(server::ServerOptions{
          .ep = {boost::asio::ip::make_address_v4(options_.listen_ip),
                 options_.listen_port},
          .http2_settings = {
              .max_concurrent_streams = 100,
              .initial_window_size = (1U << 31) - 1,
              .max_request_body_size = options_.max_request_body_size,
          }});
      http2_svr_ptr_->Start();
    });

    core_ptr_->RegisterHook(runtime::core::AfterPhase(runtime::core::Phase::kStopping), [this] {
      http2_svr_ptr_->Shutdown();
    });
  }

  asio_executor_ptr_->Start();

  core_ptr_->RegisterHook(runtime::core::AfterPhase(runtime::core::Phase::kStopping), [this] {
    asio_executor_ptr_->Shutdown();
    asio_executor_ptr_->Join();
  });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void GrpcExtension::OnShutdown() {
  if (http2_cli_pool_ptr_) {
    http2_cli_pool_ptr_->Shutdown();
  }

  if (http2_svr_ptr_) {
    http2_svr_ptr_->Shutdown();
  }

  if (asio_executor_ptr_) {
    asio_executor_ptr_->Shutdown();
    asio_executor_ptr_->Join();
  }
}

void GrpcExtension::RegisterGrpcRpcDriver() {
  std::unique_ptr<runtime::core::service::ServiceDriver> grpc_rpc_driver_ptr =
      std::make_unique<GrpcRpcDriver>(asio_executor_ptr_->IO(),
                                       http2_svr_ptr_,
                                       http2_cli_pool_ptr_);

  core_ptr_->GetServiceDispatcher().RegisterRpcDriver(std::move(grpc_rpc_driver_ptr));
}

}  // namespace ibmw::extensions::grpc_extension

IBMW_EXT_ENTRY(ibmw::extensions::grpc_extension::GrpcExtension)
