// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/service/service_dispatcher.h"
#include "engine/service/service_dispatcher_yaml.h"
#include "engine/service/inprocess_service.h"

namespace ibmw::runtime::core::service {

void ServiceDispatcher::Initialize(YAML::Node options_node) {
  RegisterInProcessServiceDriver();
  RegisterDebugLogFilter();

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Service dispatcher can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  service_catalog_ptr_ = std::make_unique<ServiceCatalog>();
  service_catalog_ptr_->SetLogger(logger_ptr_);

  service_driver_dispatcher_.SetLogger(logger_ptr_);
  service_driver_dispatcher_.SetServiceCatalog(service_catalog_ptr_.get());
  service_driver_dispatcher_.SetClientFrameworkAsyncRpcFilterBus(&client_filter_bus_);
  service_driver_dispatcher_.SetServerFrameworkAsyncRpcFilterBus(&server_filter_bus_);

  std::vector<std::string> rpc_driver_name_vec;

  // Initialize the specified driver according to the configuration
  for (auto& driver_options : options_.drivers_options) {
    auto finditr = std::find_if(
        rpc_driver_vec_.begin(), rpc_driver_vec_.end(),
        [&driver_options](const auto& ptr) {
          return ptr->Name() == driver_options.type;
        });

    MW_REQUIRE(finditr != rpc_driver_vec_.end(),
                            "Invalid service driver type '{}'",
                            driver_options.type);

    (*finditr)->SetServiceCatalog(service_catalog_ptr_.get());
    (*finditr)->Initialize(driver_options.options);

    service_driver_dispatcher_.RegisterRpcDriver(finditr->get());

    used_rpc_driver_vec_.emplace_back(finditr->get());
    rpc_driver_name_vec.emplace_back((*finditr)->Name());
  }

  // Set rules
  std::vector<std::pair<std::string, std::vector<std::string>>> client_drivers_rules;
  std::vector<std::pair<std::string, std::vector<std::string>>> client_filters_rules;
  for (const auto& item : options_.clients_options) {
    for (const auto& driver_name : item.enable_drivers) {
      MW_REQUIRE(
          std::find(rpc_driver_name_vec.begin(), rpc_driver_name_vec.end(), driver_name) != rpc_driver_name_vec.end(),
          "Invalid service driver type '{}' for func '{}'",
          driver_name, item.func_name);
    }

    client_drivers_rules.emplace_back(item.func_name, item.enable_drivers);
    client_filters_rules.emplace_back(item.func_name, item.enable_filters);
  }
  service_driver_dispatcher_.SetClientsDriversRules(client_drivers_rules);
  service_driver_dispatcher_.SetClientsFiltersRules(client_filters_rules);

  std::vector<std::pair<std::string, std::vector<std::string>>> server_drivers_rules;
  std::vector<std::pair<std::string, std::vector<std::string>>> server_filters_rules;
  for (const auto& item : options_.servers_options) {
    for (const auto& driver_name : item.enable_drivers) {
      MW_REQUIRE(
          std::find(rpc_driver_name_vec.begin(), rpc_driver_name_vec.end(), driver_name) != rpc_driver_name_vec.end(),
          "Invalid service driver type '{}' for func '{}'",
          driver_name, item.func_name);
    }

    server_drivers_rules.emplace_back(item.func_name, item.enable_drivers);
    server_filters_rules.emplace_back(item.func_name, item.enable_filters);
  }
  service_driver_dispatcher_.SetServersDriversRules(server_drivers_rules);
  service_driver_dispatcher_.SetServersFiltersRules(server_filters_rules);

  // Initialize driver bus
  service_driver_dispatcher_.Initialize();

  options_node = options_;

  MW_INFO("[RPC] initialized");
}

void ServiceDispatcher::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  service_driver_dispatcher_.Start();

  MW_INFO("[RPC] started");
}

void ServiceDispatcher::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[RPC] stopped");

  // Stop and destroy the drivers first: their threads may still be inside
  // callbacks that use the access objects, the catalog or the filters.
  service_driver_dispatcher_.Shutdown();

  rpc_driver_vec_.clear();

  service_access_map_.clear();

  service_catalog_ptr_.reset();

  server_filter_bus_.Clear();
  client_filter_bus_.Clear();

  get_executor_func_ = nullptr;
}

std::list<std::pair<std::string, std::string>> ServiceDispatcher::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  std::vector<std::vector<std::string>> client_info_table =
      {{"func", "module", "drivers", "filters"}};

  const auto& client_driver_info = service_driver_dispatcher_.GetClientsDriverInfo();
  const auto& client_index_map = service_catalog_ptr_->GetClientIndexMap();

  // Used to track entries with the same func_name
  std::unordered_map<std::string_view, size_t> client_func_indices;

  for (const auto& client_index_itr : client_index_map) {
    auto func_name = client_index_itr.first;
    auto client_driver_itr = client_driver_info.find(func_name);
    MW_REQUIRE(client_driver_itr != client_driver_info.end(),
                            "Invalid rpc catalog info.");

    auto filter_name_vec = client_filter_bus_.GetFilterNameVec(func_name);

    for (const auto& item : client_index_itr.second) {
      auto it = client_func_indices.find(func_name);

      if (it != client_func_indices.end()) {
        client_info_table[it->second][1] += "\n" + item->info.module_name;
      } else {
        std::vector<std::string> cur_client_info(4);
        cur_client_info[0] = func_name;
        cur_client_info[1] = item->info.module_name;
        cur_client_info[2] = ibmw::common::util::JoinVec(client_driver_itr->second, ",");
        cur_client_info[3] = ibmw::common::util::JoinVec(filter_name_vec, ",");
        client_info_table.emplace_back(std::move(cur_client_info));
        client_func_indices.emplace(func_name, client_info_table.size() - 1);
      }
    }
  }

  std::vector<std::vector<std::string>> server_info_table =
      {{"func", "module", "drivers", "filters"}};

  const auto& server_driver_info = service_driver_dispatcher_.GetServersDriverInfo();
  const auto& server_index_map = service_catalog_ptr_->GetServiceIndexMap();

  // Used to track entries with the same func_name
  std::unordered_map<std::string_view, size_t> server_func_indices;

  for (const auto& server_index_itr : server_index_map) {
    auto func_name = server_index_itr.first;
    auto server_driver_itr = server_driver_info.find(func_name);
    MW_REQUIRE(server_driver_itr != server_driver_info.end(),
                            "Invalid rpc catalog info.");

    auto filter_name_vec = server_filter_bus_.GetFilterNameVec(func_name);

    for (const auto& item : server_index_itr.second) {
      auto it = server_func_indices.find(func_name);

      if (it != server_func_indices.end()) {
        server_info_table[it->second][1] += "\n" + item->info.module_name;
      } else {
        std::vector<std::string> cur_server_info(4);
        cur_server_info[0] = func_name;
        cur_server_info[1] = item->info.module_name;
        cur_server_info[2] = ibmw::common::util::JoinVec(server_driver_itr->second, ",");
        cur_server_info[3] = ibmw::common::util::JoinVec(filter_name_vec, ",");
        server_info_table.emplace_back(std::move(cur_server_info));
        server_func_indices.emplace(func_name, server_info_table.size() - 1);
      }
    }
  }

  std::vector<std::string> rpc_driver_name_vec;
  rpc_driver_name_vec.reserve(rpc_driver_vec_.size());
  for (const auto& item : rpc_driver_vec_)
    rpc_driver_name_vec.emplace_back(item->Name());

  auto rpc_client_filter_name_vec = client_filter_bus_.GetAllFiltersName();
  auto rpc_server_filter_name_vec = server_filter_bus_.GetAllFiltersName();

  std::string rpc_summary;
  rpc_summary += "  drivers: " +
      (rpc_driver_name_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(rpc_driver_name_vec, ", ")) + "\n";
  rpc_summary += "  client filters: " +
      (rpc_client_filter_name_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(rpc_client_filter_name_vec, ", ")) + "\n";
  rpc_summary += "  server filters: " +
      (rpc_server_filter_name_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(rpc_server_filter_name_vec, ", ")) + "\n";

  // Append client table if non-empty
  if (client_info_table.size() > 1) {
    rpc_summary += "  clients:\n" + ibmw::common::util::FormatTree(client_info_table, true, "    ");
  }
  // Append server table if non-empty
  if (server_info_table.size() > 1) {
    rpc_summary += "  servers:\n" + ibmw::common::util::FormatTree(server_info_table, true, "    ");
  }

  std::list<std::pair<std::string, std::string>> report{
      {"RPC", rpc_summary}};

  for (const auto& driver_ptr : used_rpc_driver_vec_) {
    report.splice(report.end(), driver_ptr->GenInitializationReport());
  }

  return report;
}

void ServiceDispatcher::RegisterRpcDriver(
    std::unique_ptr<ServiceDriver>&& rpc_driver_ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  rpc_driver_vec_.emplace_back(std::move(rpc_driver_ptr));
}

void ServiceDispatcher::RegisterGetExecutorFunc(
    const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_executor_func_ = get_executor_func;
}

const ServiceAccess& ServiceDispatcher::GetRpcAccess(const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto itr = service_access_map_.find(module_info.module_name);
  if (itr != service_access_map_.end()) return *(itr->second);

  auto emplace_ret = service_access_map_.emplace(
      module_info.module_name, std::make_unique<ServiceAccess>(
                             module_info.pkg_path,
                             module_info.module_name,
                             service_driver_dispatcher_,
                             passed_context_meta_keys_));
  return *(emplace_ret.first->second);
}

void ServiceDispatcher::RegisterClientFilter(std::string_view name, FrameworkAsyncRpcFilter&& filter) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  client_filter_bus_.RegisterFilter(name, std::move(filter));
}

void ServiceDispatcher::RegisterServerFilter(std::string_view name, FrameworkAsyncRpcFilter&& filter) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  server_filter_bus_.RegisterFilter(name, std::move(filter));
}

const ServiceDriverDispatcher& ServiceDispatcher::GetServiceDriverDispatcher() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return service_driver_dispatcher_;
}

const FrameworkAsyncRpcFilterBus& ServiceDispatcher::GetClientFilterBus() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return client_filter_bus_;
}

const FrameworkAsyncRpcFilterBus& ServiceDispatcher::GetServerFilterBus() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return server_filter_bus_;
}

void ServiceDispatcher::AddPassedContextMetaKeys(const std::unordered_set<std::string>& keys) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  passed_context_meta_keys_.insert(keys.begin(), keys.end());
}

const ServiceCatalog* ServiceDispatcher::GetServiceCatalog() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return service_catalog_ptr_.get();
}

const std::vector<ServiceDriver*>& ServiceDispatcher::GetUsedRpcDriver() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return used_rpc_driver_vec_;
}

void ServiceDispatcher::RegisterInProcessServiceDriver() {
  std::unique_ptr<ServiceDriver> local_rpc_driver_ptr =
      std::make_unique<InProcessServiceDriver>();

  static_cast<InProcessServiceDriver*>(local_rpc_driver_ptr.get())
      ->SetLogger(logger_ptr_);

  static_cast<InProcessServiceDriver*>(local_rpc_driver_ptr.get())
      ->RegisterGetExecutorFunc(get_executor_func_);

  RegisterRpcDriver(std::move(local_rpc_driver_ptr));
}

}  // namespace ibmw::runtime::core::service
