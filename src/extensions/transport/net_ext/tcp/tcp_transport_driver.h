// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_tools.h"
#include "net_base/asio_tcp_cli.h"
#include "net_base/asio_tcp_svr.h"
#include "net_ext/msg_handle_catalog.h"

namespace ibmw::extensions::net_extension {

class TcpTransportDriver : public runtime::core::transport::TransportDriver {
 public:
  using TcpMsgHandleCatalog = MsgHandleCatalog<boost::asio::ip::tcp::endpoint>;

  struct Options {
    struct PubTopicOptions {
      std::string topic_name;
      std::vector<std::string> server_url_list;
    };

    std::vector<PubTopicOptions> pub_topics_options;
  };

 public:
  TcpTransportDriver(
      const std::shared_ptr<boost::asio::io_context>& io_ptr,
      const std::shared_ptr<ibmw::common::net::AsioTcpClientPool>& tcp_cli_pool_ptr,
      const std::shared_ptr<ibmw::common::net::AsioTcpServer>& tcp_svr_ptr,
      const std::shared_ptr<TcpMsgHandleCatalog>& msg_handle_catalog_ptr)
      : io_ptr_(io_ptr),
        tcp_cli_pool_ptr_(tcp_cli_pool_ptr),
        tcp_svr_ptr_(tcp_svr_ptr),
        msg_handle_catalog_ptr_(msg_handle_catalog_ptr) {}

  ~TcpTransportDriver() override = default;

  std::string_view Name() const noexcept override { return "tcp"; }
  IBMW_DECLARE_DRIVER_SLOT("ibmw.transport.tcp")

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void SetTransportCatalog(const runtime::core::transport::TransportCatalog* transport_catalog_ptr) noexcept override {
    transport_catalog_ptr_ = transport_catalog_ptr;
  }

  bool RegisterSendType(
      const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept override;
  bool OnMessage(const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept override;
  void Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept override;

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::transport::TransportCatalog* transport_catalog_ptr_ = nullptr;

  std::shared_ptr<boost::asio::io_context> io_ptr_;
  std::shared_ptr<ibmw::common::net::AsioTcpClientPool> tcp_cli_pool_ptr_;
  std::shared_ptr<ibmw::common::net::AsioTcpServer> tcp_svr_ptr_;
  std::shared_ptr<TcpMsgHandleCatalog> msg_handle_catalog_ptr_;

  std::unordered_map<
      std::string,
      std::unique_ptr<ibmw::runtime::core::transport::MessageHandlerTool>>
      tcp_message_handler_map_;

  struct PubCfgInfo {
    std::vector<boost::asio::ip::tcp::endpoint> server_ep_vec;
  };
  std::unordered_map<std::string_view, PubCfgInfo> pub_cfg_info_map_;
};

}  // namespace ibmw::extensions::net_extension
