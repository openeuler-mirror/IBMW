// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/service/service_driver_dispatcher.h"
#include <regex>
#include <vector>
#include "c/service/rpc_context_base.h"

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_status.h"
#include "engine/service/service_driver_tools.h"

namespace ibmw::runtime::core::service {

void ServiceDriverDispatcher::Initialize() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Service driver bus can only be initialized once.");
}

void ServiceDriverDispatcher::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  for (auto& driver : rpc_driver_index_vec_) {
    MW_TRACE("Start service driver '{}'.", driver->Name());
    driver->Start();
  }
}

void ServiceDriverDispatcher::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  size_t error_count = 0;
  std::string first_error;
  for (auto& driver : rpc_driver_index_vec_) {
    MW_TRACE("Shutdown service driver '{}'.", driver->Name());
    try {
      driver->Shutdown();
    } catch (const std::exception& e) {
      ++error_count;
      if (first_error.empty()) first_error = e.what();
      MW_ERROR("Shutdown service driver '{}' failed: {}", driver->Name(),
               e.what());
    } catch (...) {
      ++error_count;
      if (first_error.empty()) first_error = "unknown exception";
      MW_ERROR("Shutdown service driver '{}' failed: unknown exception",
               driver->Name());
    }
  }
  if (error_count != 0) {
    MW_ERROR("Service driver shutdown completed with {} error(s); first error: {}",
             error_count, first_error);
  }
}

void ServiceDriverDispatcher::SetServiceCatalog(ServiceCatalog* service_catalog_ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  service_catalog_ptr_ = service_catalog_ptr;
}

void ServiceDriverDispatcher::SetClientFrameworkAsyncRpcFilterBus(
    FrameworkAsyncRpcFilterBus* ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  client_filter_bus_ptr_ = ptr;
}

void ServiceDriverDispatcher::SetServerFrameworkAsyncRpcFilterBus(
    FrameworkAsyncRpcFilterBus* ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  server_filter_bus_ptr_ = ptr;
}

void ServiceDriverDispatcher::SetClientsFiltersRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  clients_filters_rules_ = rules;
}

void ServiceDriverDispatcher::SetServersFiltersRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  servers_filters_rules_ = rules;
}

void ServiceDriverDispatcher::SetClientsDriversRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  clients_drivers_rules_ = rules;
}

void ServiceDriverDispatcher::SetServersDriversRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  servers_drivers_rules_ = rules;
}

void ServiceDriverDispatcher::RegisterRpcDriver(ServiceDriver* rpc_driver_ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  rpc_driver_index_vec_.emplace_back(rpc_driver_ptr);
  rpc_driver_index_map_.emplace(rpc_driver_ptr->Name(), rpc_driver_ptr);
}

bool ServiceDriverDispatcher::RegisterServiceFunc(RegisterServiceFuncAccessInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Service func can only be registered when state is 'Init'.");
    return false;
  }

  if (wrapper.req_type_support == nullptr || wrapper.rsp_type_support == nullptr) [[unlikely]] {
    MW_ERROR("Msg type support is null.");
    return false;
  }

  if (wrapper.service_func == nullptr) [[unlikely]] {
    MW_ERROR("Service func is null.");
    return false;
  }

  auto func_name = ibmw::util::ToStdStringView(wrapper.func_name);

  // Create func wrapper
  auto service_func_wrapper_ptr = std::make_unique<ServiceFuncWrapper>();
  service_func_wrapper_ptr->info = ServiceInfo{
      .func_name = std::string(func_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++server_index_,
      .custom_type_support_ptr = wrapper.custom_type_support_ptr,
      .req_type_support_ref = ibmw::util::TypeSupportRef(wrapper.req_type_support),
      .rsp_type_support_ref = ibmw::util::TypeSupportRef(wrapper.rsp_type_support)};

  // Create filter
  auto filter_name_vec = GetFilterRules(func_name, servers_filters_rules_);
  server_filter_bus_ptr_->CreateFilterCollectorIfNotExist(func_name, filter_name_vec);

  // Register func wrapper
  const auto& filter_collector = server_filter_bus_ptr_->GetFilterCollector(func_name);

  auto service_func_shared_ptr = std::make_shared<ibmw::rpc::ServiceFunc>(wrapper.service_func);

  service_func_wrapper_ptr->service_func =
      [&filter_collector, service_func_shared_ptr](
          const std::shared_ptr<ServiceMessageFrame>& invoke_wrapper_ptr) {
        filter_collector.InvokeRpc(
            [service_func_ptr = service_func_shared_ptr.get()](
                const std::shared_ptr<ServiceMessageFrame>& invoke_wrapper_ptr) {
              auto callback = std::move(invoke_wrapper_ptr->callback);
              invoke_wrapper_ptr->callback = nullptr;
              auto service_callback = std::make_shared<ibmw::rpc::ServiceCallback>(
                  [callback{std::move(callback)}](uint32_t status) {
                    callback(ibmw::rpc::Status(status));
                  });

              // set request id into context if available
              auto& ctx_ref = invoke_wrapper_ptr->ctx_ref;
              ctx_ref.SetMetaValue(IBMW_RPC_CONTEXT_KEY_RESPONSE_ID, ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_REQUEST_ID));

              (*service_func_ptr)(
                  invoke_wrapper_ptr->ctx_ref.NativeHandle(),
                  invoke_wrapper_ptr->req_ptr,
                  invoke_wrapper_ptr->rsp_ptr,
                  service_callback->NativeHandle());
            },
            invoke_wrapper_ptr);
      };

  const auto& service_func_wrapper_ref = *service_func_wrapper_ptr;

  if (!service_catalog_ptr_->RegisterServiceFunc(std::move(service_func_wrapper_ptr)))
    return false;

  auto driver_itr = servers_driver_index_map_.find(func_name);
  if (driver_itr == servers_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(func_name, servers_drivers_rules_);
    auto emplace_ret = servers_driver_index_map_.emplace(func_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : driver_itr->second) {
    MW_TRACE("Register service func '{}' to driver '{}'.", func_name, itr->Name());
    ret &= itr->RegisterServiceFunc(service_func_wrapper_ref);
  }
  return ret;
}

bool ServiceDriverDispatcher::RegisterClientFunc(RegisterClientFuncAccessInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Client func can only be registered when state is 'Init'.");
    return false;
  }

  if (wrapper.req_type_support == nullptr || wrapper.rsp_type_support == nullptr) [[unlikely]] {
    MW_ERROR("Msg type support is null.");
    return false;
  }

  auto func_name = ibmw::util::ToStdStringView(wrapper.func_name);

  // Create func wrapper
  auto client_func_wrapper_ptr = std::make_unique<ClientFuncWrapper>();
  client_func_wrapper_ptr->info = ServiceInfo{
      .func_name = std::string(func_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++client_index_,
      .custom_type_support_ptr = wrapper.custom_type_support_ptr,
      .req_type_support_ref = ibmw::util::TypeSupportRef(wrapper.req_type_support),
      .rsp_type_support_ref = ibmw::util::TypeSupportRef(wrapper.rsp_type_support)};

  // initialize request_id for this func  (only in Init state)
  request_id_map_.try_emplace(std::string(func_name), 0);

  // Create filter
  auto filter_name_vec = GetFilterRules(func_name, clients_filters_rules_);
  client_filter_bus_ptr_->CreateFilterCollectorIfNotExist(func_name, filter_name_vec);

  // Register func wrapper
  const auto& client_func_wrapper_ref = *client_func_wrapper_ptr;

  if (!service_catalog_ptr_->RegisterClientFunc(std::move(client_func_wrapper_ptr)))
    return false;

  auto driver_itr = clients_driver_index_map_.find(func_name);
  if (driver_itr == clients_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(func_name, clients_drivers_rules_);
    auto emplace_ret = clients_driver_index_map_.emplace(func_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : driver_itr->second) {
    MW_TRACE("Register client func '{}' to driver '{}'.", func_name, itr->Name());
    ret &= itr->RegisterClientFunc(client_func_wrapper_ref);
  }
  return ret;
}

void ServiceDriverDispatcher::Call(CallAccessInfoWrapper&& wrapper) {
  MW_REQUIRE(state_.load() == State::kStart, "Method can only be called when state is 'Start'.");

  auto func_name = util::ToStdStringView(wrapper.func_name);
  auto client_callback_ptr = std::make_shared<ibmw::rpc::ClientCallback>(wrapper.callback);
  auto& client_callback = *client_callback_ptr;
  ibmw::rpc::ContextRef ctx_ref(wrapper.ctx_ptr);

  // Find registered func
  const auto* client_func_wrapper_ptr = service_catalog_ptr_->GetClientFuncWrapperPtr(
      func_name, wrapper.pkg_path, wrapper.module_name);

  // If func is not registered
  if (client_func_wrapper_ptr == nullptr) [[unlikely]] {
    MW_WARN("Func is not registered, func: {}, pkg: {}, module: {}",
               func_name, wrapper.pkg_path, wrapper.module_name);

    client_callback(IBMW_RPC_STATUS_CLI_FUNC_NOT_REGISTERED);
    return;
  }

  // Check ctx
  if (ctx_ref.GetType() != ibmw_rpc_context_type_t::IBMW_RPC_CLIENT_CONTEXT ||
      ctx_ref.CheckUsed()) {
    client_callback(IBMW_RPC_STATUS_CLI_INVALID_CONTEXT);
    return;
  }

  ctx_ref.SetUsed();

  // set request id into context if available
  auto it = request_id_map_.find(func_name);
  if (it != request_id_map_.end()) {
    uint32_t seq = ++(it->second);
    ctx_ref.SetMetaValue(IBMW_RPC_CONTEXT_KEY_REQUEST_ID, std::to_string(seq));
  }

  if (ctx_ref.GetSerializationType().empty()) {
    const auto& req_ts = client_func_wrapper_ptr->info.req_type_support_ref;
    if (req_ts.SerializationTypesSupportedNum() > 0) {
      ctx_ref.SetSerializationType(req_ts.DefaultSerializationType());
    }
    // If no serialization types supported (e.g. ROS2 types), leave empty.
    // The local driver will use copy-based delivery instead of serialization.
  }

  // Find filter
  const auto& filter_collector = client_filter_bus_ptr_->GetFilterCollector(func_name);

  // Create a wrapper
  auto client_call_wrapper_ptr = std::make_shared<ServiceMessageFrame>(
      ServiceMessageFrame{
          .info = client_func_wrapper_ptr->info,
          .req_ptr = wrapper.req_ptr,
          .rsp_ptr = wrapper.rsp_ptr,
          .ctx_ref = ctx_ref});

  client_call_wrapper_ptr->callback =
      [client_call_wrapper_ptr, client_callback_ptr{std::move(client_callback_ptr)}](ibmw::rpc::Status status) mutable {
        (*client_callback_ptr)(status.Code());
      };

  // Initiate a call
  filter_collector.InvokeRpc(
      [this](const std::shared_ptr<ServiceMessageFrame>& client_call_wrapper_ptr) {
        // When timeout is not set, the default 5s timeout
        if (client_call_wrapper_ptr->ctx_ref.Timeout().count() == 0) {
          client_call_wrapper_ptr->ctx_ref.SetTimeout(std::chrono::seconds(5));
        }

        std::string_view func_name = client_call_wrapper_ptr->info.func_name;

        auto find_itr = clients_driver_index_map_.find(func_name);

        if (find_itr == clients_driver_index_map_.end()) [[unlikely]] {
          MW_WARN("Service call found no driver to handle, func name '{}'.", func_name);
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_NO_DRIVER_TO_HANDLE));
          return;
        }

        const auto& driver_ptr_vec = find_itr->second;

        if (driver_ptr_vec.empty()) [[unlikely]] {
          MW_WARN("Service call found no driver to handle, func name '{}'.", func_name);
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_NO_DRIVER_TO_HANDLE));
          return;
        }

        // If a driver is specified in ctx, the specified driver is used
        std::string_view to_addr(client_call_wrapper_ptr->ctx_ref.GetToAddr());
        if (!to_addr.empty()) {
          // to_addr format: driver_name://url_str
          MW_TRACE("Service call use the specified address '{}', func name '{}'.", to_addr, func_name);
          auto pos = to_addr.find("://");
          if (pos != std::string_view::npos) {
            auto addr_driver = to_addr.substr(0, pos);

            auto driver_itr = rpc_driver_index_map_.find(addr_driver);
            if (driver_itr != rpc_driver_index_map_.end()) {
              auto* driver_ptr = driver_itr->second;

              if (std::find(driver_ptr_vec.begin(), driver_ptr_vec.end(), driver_ptr) != driver_ptr_vec.end()) {
                driver_ptr->Call(client_call_wrapper_ptr);
                return;
              }
            }
          }
          MW_ERROR("Service call address '{}' is invalid, func name '{}'.", to_addr, func_name);
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_ADDR));
          return;
        }

        // Use the first driver of the configuration
        auto& driver = *(driver_ptr_vec[0]);
        MW_TRACE("Service call use driver '{}', func name '{}'.", driver.Name(), func_name);
        client_call_wrapper_ptr->ctx_ref.SetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER, driver.Name());
        driver.Call(client_call_wrapper_ptr);
      },
      client_call_wrapper_ptr);
}

ServiceDriverDispatcher::FuncDriverInfoMap ServiceDriverDispatcher::GetClientsDriverInfo() const {
  std::unordered_map<std::string_view, std::vector<std::string_view>> result;
  for (const auto& itr : clients_driver_index_map_) {
    std::vector<std::string_view> drivers_name;
    drivers_name.reserve(itr.second.size());
    for (const auto& item : itr.second)
      drivers_name.emplace_back(item->Name());

    result.emplace(itr.first, std::move(drivers_name));
  }

  return result;
}

ServiceDriverDispatcher::FuncDriverInfoMap ServiceDriverDispatcher::GetServersDriverInfo() const {
  std::unordered_map<std::string_view, std::vector<std::string_view>> result;
  for (const auto& itr : servers_driver_index_map_) {
    std::vector<std::string_view> drivers_name;
    for (const auto& item : itr.second)
      drivers_name.emplace_back(item->Name());

    result.emplace(itr.first, std::move(drivers_name));
  }

  return result;
}

std::vector<ServiceDriver*> ServiceDriverDispatcher::GetDriversByRules(
    std::string_view func_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  for (const auto& item : rules) {
    const auto& func_regex = item.first;
    const auto& enable_drivers = item.second;

    try {
      if (std::regex_match(func_name.begin(), func_name.end(), std::regex(func_regex, std::regex::ECMAScript))) {
        std::vector<ServiceDriver*> driver_ptr_vec;

        for (const auto& driver_name : enable_drivers) {
          auto itr = std::find_if(
              rpc_driver_index_vec_.begin(), rpc_driver_index_vec_.end(),
              [&driver_name](const ServiceDriver* driver_ptr) -> bool {
                return driver_ptr->Name() == driver_name;
              });

          if (itr == rpc_driver_index_vec_.end()) [[unlikely]] {
            MW_WARN("Can not find '{}' in driver list.", driver_name);
            continue;
          }

          driver_ptr_vec.emplace_back(*itr);
        }

        return driver_ptr_vec;
      }
    } catch (const std::exception& e) {
      MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                 func_regex, func_name, e.what());
    }
  }

  return {};
}

std::vector<std::string> ServiceDriverDispatcher::GetFilterRules(
    std::string_view func_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  for (const auto& item : rules) {
    const auto& func_regex = item.first;
    const auto& filters = item.second;

    try {
      if (std::regex_match(func_name.begin(), func_name.end(), std::regex(func_regex, std::regex::ECMAScript))) {
        return filters;
      }
    } catch (const std::exception& e) {
      MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                 func_regex, func_name, e.what());
    }
  }

  return {};
}

}  // namespace ibmw::runtime::core::service
