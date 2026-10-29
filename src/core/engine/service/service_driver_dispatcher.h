// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>

#include "c/util/function_base.h"
#include "cpp/service/rpc_handle.h"
#include "engine/service/service_driver.h"
#include "engine/service/service_framework_async_filter.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::service {

struct RegisterServiceFuncAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;

  ibmw_string_view_t func_name;
  const void* custom_type_support_ptr;
  const ibmw_type_support_base_t* req_type_support;
  const ibmw_type_support_base_t* rsp_type_support;
  ibmw_function_base_t* service_func;
};

struct RegisterClientFuncAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;

  ibmw_string_view_t func_name;
  const void* custom_type_support_ptr;
  const ibmw_type_support_base_t* req_type_support;
  const ibmw_type_support_base_t* rsp_type_support;
};

struct CallAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;

  ibmw_string_view_t func_name;
  const ibmw_service_context_base_t* ctx_ptr;
  const void* req_ptr;
  void* rsp_ptr;
  ibmw_function_base_t* callback;
};

class ServiceDriverDispatcher {
 public:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  ServiceDriverDispatcher()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~ServiceDriverDispatcher() = default;

  ServiceDriverDispatcher(const ServiceDriverDispatcher&) = delete;
  ServiceDriverDispatcher& operator=(const ServiceDriverDispatcher&) = delete;

  void Initialize();
  void Start();
  void Shutdown();

  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void SetServiceCatalog(ServiceCatalog* service_catalog_ptr);

  void SetClientsFiltersRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);
  void SetServersFiltersRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  void SetClientFrameworkAsyncRpcFilterBus(FrameworkAsyncRpcFilterBus* ptr);
  void SetServerFrameworkAsyncRpcFilterBus(FrameworkAsyncRpcFilterBus* ptr);

  void SetClientsDriversRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);
  void SetServersDriversRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  void RegisterRpcDriver(ServiceDriver* rpc_driver_ptr);

  bool RegisterServiceFunc(RegisterServiceFuncAccessInfoWrapper&& wrapper);
  bool RegisterClientFunc(RegisterClientFuncAccessInfoWrapper&& wrapper);
  void Call(CallAccessInfoWrapper&& wrapper);

  using FuncDriverInfoMap = std::unordered_map<std::string_view, std::vector<std::string_view>>;
  FuncDriverInfoMap GetClientsDriverInfo() const;
  FuncDriverInfoMap GetServersDriverInfo() const;

 private:
  void CallImpl(CallAccessInfoWrapper& wrapper,
                std::shared_ptr<ibmw::rpc::ClientCallback>& client_callback_ptr,
                std::shared_ptr<ServiceMessageFrame>& client_call_wrapper_ptr);
  void FailCall(const std::shared_ptr<ibmw::rpc::ClientCallback>& client_callback_ptr,
                const std::shared_ptr<ServiceMessageFrame>& client_call_wrapper_ptr) noexcept;

  std::vector<ServiceDriver*> GetDriversByRules(
      std::string_view func_name,
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  std::vector<std::string> GetFilterRules(
      std::string_view func_name,
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

 private:
  std::atomic<State> state_ = State::kPreInit;
  uint64_t client_index_ = 0;
  uint64_t server_index_ = 0;

  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  ServiceCatalog* service_catalog_ptr_ = nullptr;

  // filter
  std::vector<std::pair<std::string, std::vector<std::string>>> clients_filters_rules_;
  std::vector<std::pair<std::string, std::vector<std::string>>> servers_filters_rules_;

  FrameworkAsyncRpcFilterBus* client_filter_bus_ptr_ = nullptr;
  FrameworkAsyncRpcFilterBus* server_filter_bus_ptr_ = nullptr;

  // driver
  std::vector<ServiceDriver*> rpc_driver_index_vec_;
  std::unordered_map<std::string_view, ServiceDriver*> rpc_driver_index_map_;

  std::vector<std::pair<std::string, std::vector<std::string>>> clients_drivers_rules_;
  std::vector<std::pair<std::string, std::vector<std::string>>> servers_drivers_rules_;

  std::unordered_map<
      std::string,
      std::vector<ServiceDriver*>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      clients_driver_index_map_;

  std::unordered_map<
      std::string,
      std::vector<ServiceDriver*>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      servers_driver_index_map_;

  std::unordered_map<
      std::string,
      std::atomic<uint32_t>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      request_id_map_;
};
}  // namespace ibmw::runtime::core::service
