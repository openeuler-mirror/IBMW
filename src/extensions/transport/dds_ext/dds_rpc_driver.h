// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#if __has_include(<fastdds/rtps/common/WriteParams.hpp>)
#include <fastdds/rtps/common/WriteParams.hpp>
#else
#include <fastdds/rtps/common/WriteParams.h>
#endif

#include "engine/service/service_driver.h"
#include "extension_api/ibmw_service_client_readiness.h"
#include "engine/service/service_driver_tools.h"
#include "engine/rpc_client_tool.h"
#include "cpp/executor/executor.h"
#include "dds_ext/dds_bus.h"
#include "dds_ext/global.h"
#include "dds_ext/fastdds_compat.h"

namespace ibmw::extensions::dds_extension {

#if IBMW_FASTDDS_V3_PLUS
namespace dds_rpc_rtps = fastdds_compat::rtps;
namespace dds_rpc_return = fastdds_compat::return_code;
inline constexpr auto kDdsRpcReturnOk = fastdds_compat::kReturnCodeOk;
#else
namespace dds_rpc_rtps = fastdds_compat::rtps;
namespace dds_rpc_return = fastdds_compat::return_code;
inline constexpr auto kDdsRpcReturnOk = fastdds_compat::kReturnCodeOk;
#endif

inline bool DdsRpcWriteSucceeded(bool ret) {
  return ret;
}

inline bool DdsRpcWriteSucceeded(dds_rpc_return::ReturnCode_t ret) {
  return ret == kDdsRpcReturnOk;
}

class DdsRpcDriver : public runtime::core::service::ServiceDriver,
                      public ibmw::extension::ServiceClientReadinessV1 {
 public:
  struct Options {
    bool ros2_compatible = true;
    std::string timeout_executor;
  };

 public:
  explicit DdsRpcDriver(DdsBus& dds_bus);
  ~DdsRpcDriver() override = default;

  std::string_view Name() const noexcept override { return "dds"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;
  bool IsClientReady(std::string_view module, std::string_view function) const noexcept override;

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept override;

  void SetServiceCatalog(const runtime::core::service::ServiceCatalog* service_catalog_ptr) noexcept override {
    service_catalog_ptr_ = service_catalog_ptr;
  }

  bool RegisterServiceFunc(
      const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept override;
  bool RegisterClientFunc(
      const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept override;
  void Call(
      const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept override;

  using GetExecutorFunc = std::function<ibmw::executor::ExecutorHandle(std::string_view)>;
  void RegisterGetExecutorFunc(GetExecutorFunc func) { get_executor_func_ = std::move(func); }

 private:
  // ----------- Server endpoint (one per service func) -----------
  struct ServerEndpoint {
    std::string func_name;
    std::string dds_topic_rq;
    std::string dds_topic_rr;

    eprosima::fastdds::dds::Topic* topic_rq = nullptr;
    eprosima::fastdds::dds::Topic* topic_rr = nullptr;
    eprosima::fastdds::dds::DataReader* reader = nullptr;
    eprosima::fastdds::dds::DataWriter* writer = nullptr;

    const runtime::core::service::ServiceFuncWrapper* service_func_wrapper = nullptr;
  };

  // ----------- Client endpoint (one per client func) -----------
  struct ClientEndpoint {
    std::string func_name;
    std::string dds_topic_rq;
    std::string dds_topic_rr;

    eprosima::fastdds::dds::Topic* topic_rq = nullptr;
    eprosima::fastdds::dds::Topic* topic_rr = nullptr;
    eprosima::fastdds::dds::DataWriter* writer = nullptr;
    eprosima::fastdds::dds::DataReader* reader = nullptr;

    // Reply DataReader GUID — needed to build SampleIdentity for requests
    dds_rpc_rtps::GUID_t reply_reader_guid;

    // Sequence counter for matching requests with responses
    std::atomic<uint32_t> seq_counter{0};

    runtime::core::util::RpcClientTool<std::shared_ptr<runtime::core::service::ServiceMessageFrame>>
        client_tool;

    const runtime::core::service::ClientFuncWrapper* client_func_wrapper = nullptr;
  };

  // ----------- Listeners -----------
  class ServerListener : public eprosima::fastdds::dds::DataReaderListener {
   public:
    ServerListener(DdsRpcDriver& driver, ServerEndpoint& ep) : driver_(driver), ep_(ep) {}
    void on_data_available(eprosima::fastdds::dds::DataReader* reader) override;

   private:
    DdsRpcDriver& driver_;
    ServerEndpoint& ep_;
  };

  class ClientListener : public eprosima::fastdds::dds::DataReaderListener {
   public:
    ClientListener(DdsRpcDriver& driver, ClientEndpoint& ep) : driver_(driver), ep_(ep) {}
    void on_data_available(eprosima::fastdds::dds::DataReader* reader) override;

   private:
    DdsRpcDriver& driver_;
    ClientEndpoint& ep_;
  };

  // ----------- Helpers -----------
  std::string GetRequestTopicName(const std::string& func_name) const;
  std::string GetReplyTopicName(const std::string& func_name) const;

  /// Extract the service name from func_name for DDS topic naming.
  /// e.g. "ros2:/add_two_ints" -> "/add_two_ints"
  static std::string ExtractServiceName(const std::string& func_name);

  eprosima::fastdds::dds::Topic* GetOrCreateTopic(
      const std::string& topic_name,
      eprosima::fastdds::dds::TypeSupport type_support);

 private:
  enum class State : uint32_t { kPreInit, kInit, kStart, kShutdown };

  DdsBus& dds_bus_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::service::ServiceCatalog* service_catalog_ptr_ = nullptr;

  GetExecutorFunc get_executor_func_;
  ibmw::executor::ExecutorHandle timeout_executor_;

  eprosima::fastdds::dds::Publisher* publisher_ = nullptr;
  eprosima::fastdds::dds::Subscriber* subscriber_ = nullptr;

  // server_key -> ServerEndpoint
  std::unordered_map<std::string, std::unique_ptr<ServerEndpoint>> server_endpoints_;
  std::unordered_map<std::string, std::unique_ptr<ServerListener>> server_listeners_;

  // client_key -> ClientEndpoint
  std::unordered_map<std::string, std::unique_ptr<ClientEndpoint>> client_endpoints_;
  std::unordered_map<std::string, std::unique_ptr<ClientListener>> client_listeners_;

  // topic_name -> Topic*
  std::unordered_map<std::string, eprosima::fastdds::dds::Topic*> topic_map_;
};

}  // namespace ibmw::extensions::dds_extension
