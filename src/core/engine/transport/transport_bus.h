// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cpp/executor/executor.h"
#include "engine/isubsystem.h"
#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_capabilities.h"
#include "engine/transport/transport_access.h"
#include "engine/core_types.h"

namespace ibmw::runtime::core::transport {

class TransportBus : public ISubsystem {
 public:
  struct Options {
    struct DriverOptions {
      std::string type;
      YAML::Node options;
    };
    std::vector<DriverOptions> drivers_options;

    struct PubTopicOptions {
      std::string topic_name;
      std::vector<std::string> enable_drivers;
      std::vector<std::string> enable_filters;
    };
    std::vector<PubTopicOptions> pub_topics_options;

    struct SubTopicOptions {
      std::string topic_name;
      std::vector<std::string> enable_drivers;
      std::vector<std::string> enable_filters;
    };
    std::vector<SubTopicOptions> sub_topics_options;
  };

 public:
  TransportBus() = default;
  ~TransportBus() override = default;

  TransportBus(const TransportBus&) = delete;
  TransportBus& operator=(const TransportBus&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;
  bool CanShutdownFromCurrentThread() const noexcept;
  bool TryPrepareShutdown();
  bool TryShutdown();

  const Options& GetOptions() const { return options_; }

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

  void RegisterTransportDriver(std::unique_ptr<TransportDriver>&& transport_driver_ptr);
  void RegisterTransportDriver(
      std::unique_ptr<TransportDriver>&& transport_driver_ptr,
      TransportDriverCapabilitiesV1 capabilities);

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func);

  const TransportAccess& GetBusAccess(const util::ModuleDetailInfo& module_info);
  const TransportAccess& GetBusAccess(std::string_view module_name = "core") {
    return GetBusAccess(util::ModuleDetailInfo{.module_name = std::string(module_name), .pkg_path = "core"});
  }

  void RegisterPublishFilter(std::string_view name, FrameworkAsyncTransportFilter&& filter);
  void RegisterSubscribeFilter(std::string_view name, FrameworkAsyncTransportFilter&& filter);

  void AddPassedContextMetaKeys(const std::unordered_set<std::string>& keys);

  bool OnMessage(MessageHandlerWrapper&& wrapper) {
    return transport_driver_dispatcher_.OnMessage(std::move(wrapper));
  }
  bool RegisterSendType(SendTypeWrapper&& wrapper) {
    return transport_driver_dispatcher_.RegisterSendType(std::move(wrapper));
  }
  void Send(MessageFrame&& wrapper) {
    transport_driver_dispatcher_.Send(std::move(wrapper));
  }

  const TransportCatalog* GetTransportCatalog() const;
  const std::vector<TransportDriver*>& GetUsedTransportDriver() const;
  const TransportDriverDispatcher& GetTransportDriverDispatcher() const;

  const FrameworkAsyncTransportFilterBus& GetPublishFilterBus() const;
  const FrameworkAsyncTransportFilterBus& GetSubscribeFilterBus() const;

 private:
  void RegisterInProcessTransport();
  void RegisterDebugLogFilter();
  bool CanShutdownFromCurrentThreadUnlocked() const noexcept;
  bool TryPrepareShutdownUnlocked();

 private:
  Options options_;

  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;

  std::unordered_set<std::string> passed_context_meta_keys_;

  FrameworkAsyncTransportFilterBus publish_filter_bus_;
  FrameworkAsyncTransportFilterBus subscribe_filter_bus_;

  std::unique_ptr<TransportCatalog> transport_catalog_ptr_;

  std::vector<std::unique_ptr<TransportDriver>> transport_driver_vec_;
  std::vector<TransportDriver*> used_transport_driver_vec_;
  std::unordered_map<const TransportDriver*, TransportDriverCapabilitiesV1>
      transport_driver_capabilities_map_;

  TransportDriverDispatcher transport_driver_dispatcher_;

  class TransportAccessWrap {
   public:
    TransportAccessWrap(
        std::string_view input_pkg_path,
        std::string_view input_module_name,
        ibmw::common::util::LoggerWrapper& logger,
        TransportDriverDispatcher& transport_driver_bus,
        const std::unordered_set<std::string>& passed_context_meta_keys,
        std::atomic_bool& transport_access_start_flag)
        : pkg_path(input_pkg_path),
          module_name(input_module_name),
          transport_handle_access(
              pkg_path,
              module_name,
              logger,
              transport_driver_bus,
              passed_context_meta_keys,
              transport_access_start_flag,
              publisher_access_map,
              subscriber_access_map) {}

    const std::string pkg_path;
    const std::string module_name;

    TransportAccess::SenderAccessMap publisher_access_map;
    TransportAccess::ReceiverAccessMap subscriber_access_map;
    TransportAccess transport_handle_access;
  };

  std::atomic_bool transport_access_start_flag_ = false;
  mutable std::mutex shutdown_mutex_;
  bool shutdown_prepared_ = false;
  std::unordered_set<const TransportDriver*> prepared_shutdown_drivers_;
  std::unordered_map<std::string, std::unique_ptr<TransportAccessWrap>> transport_access_wrap_map_;
};

}  // namespace ibmw::runtime::core::transport
