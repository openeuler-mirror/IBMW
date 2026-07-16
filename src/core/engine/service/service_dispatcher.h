// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/executor/executor.h"
#include "engine/isubsystem.h"
#include "engine/service/service_driver_dispatcher.h"
#include "engine/service/service_access.h"
#include "engine/core_types.h"

namespace ibmw::runtime::core::service {

class ServiceDispatcher : public ISubsystem {
 public:
  struct Options {
    struct DriverOptions {
      std::string type;
      YAML::Node options;
    };
    std::vector<DriverOptions> drivers_options;

    struct ClientOptions {
      std::string func_name;
      std::vector<std::string> enable_drivers;
      std::vector<std::string> enable_filters;
    };
    std::vector<ClientOptions> clients_options;

    struct ServerOptions {
      std::string func_name;
      std::vector<std::string> enable_drivers;
      std::vector<std::string> enable_filters;
    };
    std::vector<ServerOptions> servers_options;
  };

 public:
  ServiceDispatcher() = default;
  ~ServiceDispatcher() override = default;

  ServiceDispatcher(const ServiceDispatcher&) = delete;
  ServiceDispatcher& operator=(const ServiceDispatcher&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  const Options& GetOptions() const { return options_; }

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

  void RegisterRpcDriver(std::unique_ptr<ServiceDriver>&& rpc_driver_ptr);

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func);

  const ServiceAccess& GetRpcAccess(const util::ModuleDetailInfo& module_info);
  const ServiceAccess& GetRpcAccess(std::string_view module_name = "core") {
    return GetRpcAccess(util::ModuleDetailInfo{.module_name = std::string(module_name), .pkg_path = "core"});
  }

  void RegisterClientFilter(std::string_view name, FrameworkAsyncRpcFilter&& filter);
  void RegisterServerFilter(std::string_view name, FrameworkAsyncRpcFilter&& filter);

  void AddPassedContextMetaKeys(const std::unordered_set<std::string>& keys);

  const ServiceCatalog* GetServiceCatalog() const;
  const std::vector<ServiceDriver*>& GetUsedRpcDriver() const;
  const ServiceDriverDispatcher& GetServiceDriverDispatcher() const;
  const FrameworkAsyncRpcFilterBus& GetClientFilterBus() const;
  const FrameworkAsyncRpcFilterBus& GetServerFilterBus() const;

 private:
  void RegisterInProcessServiceDriver();
  void RegisterDebugLogFilter();

 private:
  Options options_;

  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;

  std::unordered_set<std::string> passed_context_meta_keys_;

  FrameworkAsyncRpcFilterBus client_filter_bus_;
  FrameworkAsyncRpcFilterBus server_filter_bus_;

  std::unique_ptr<ServiceCatalog> service_catalog_ptr_;

  std::vector<std::unique_ptr<ServiceDriver>> rpc_driver_vec_;
  std::vector<ServiceDriver*> used_rpc_driver_vec_;

  ServiceDriverDispatcher service_driver_dispatcher_;

  std::unordered_map<std::string, std::unique_ptr<ServiceAccess>> service_access_map_;
};

}  // namespace ibmw::runtime::core::service
