// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/module/module_bus.h"

#include <algorithm>

#include "cpp/module_base.h"
#include "cpp/util/string.h"
#include "engine/logger/log_level_tool.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::module::ModuleBus::Options> {
  using Options = ibmw::runtime::core::module::ModuleBus::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["packages"] = YAML::Node();
    for (const auto& pkg_options : rhs.pkgs_options) {
      Node pkg_options_node;
      pkg_options_node["path"] = pkg_options.path;
      pkg_options_node["disable_modules"] = pkg_options.disable_modules;
      pkg_options_node["enable_modules"] = pkg_options.enable_modules;
      node["packages"].push_back(pkg_options_node);
    }

    node["modules"] = YAML::Node();
    for (const auto& module_options : rhs.modules_options) {
      Node module_options_node;
      module_options_node["name"] = module_options.name;
      module_options_node["enable"] = module_options.enable;
      if (!module_options.use_default_log_lvl)
        module_options_node["log_level"] =
            ibmw::runtime::core::logger::LogLevelTool::GetLogLevelName(module_options.log_lvl);

      if (!module_options.cfg_file_path.empty())
        module_options_node["cfg_file_path"] = module_options.cfg_file_path;

      node["modules"].push_back(module_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["packages"] && node["packages"].IsSequence()) {
      for (const auto& pkg_options_node : node["packages"]) {
        Options::PkgLoaderOptions pkg_options{
            .path = pkg_options_node["path"].as<std::string>()};

        if (pkg_options_node["disable_modules"])
          pkg_options.disable_modules =
              pkg_options_node["disable_modules"].as<std::vector<std::string>>();

        if (pkg_options_node["enable_modules"])
          pkg_options.enable_modules =
              pkg_options_node["enable_modules"].as<std::vector<std::string>>();

        rhs.pkgs_options.emplace_back(std::move(pkg_options));
      }
    }

    if (node["modules"] && node["modules"].IsSequence()) {
      for (const auto& module_options_node : node["modules"]) {
        Options::ModuleOptions module_options{
            .name = module_options_node["name"].as<std::string>()};

        if (module_options_node["enable"])
          module_options.enable = module_options_node["enable"].as<bool>();

        if (module_options_node["log_level"]) {
          module_options.use_default_log_lvl = false;
          module_options.log_lvl = ibmw::runtime::core::logger::LogLevelTool::GetLogLevelFromName(
              module_options_node["log_level"].as<std::string>());
        }

        if (module_options_node["cfg_file_path"]) {
          module_options.cfg_file_path = module_options_node["cfg_file_path"].as<std::string>();
        }

        rhs.modules_options.emplace_back(std::move(module_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::module {

void ModuleBus::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      module_access_configurator_,
      "Module access configurator is not set before initialize.");

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Module bus can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  // Load all dynamic libraries
  for (auto& pkg_options : options_.pkgs_options) {
    auto module_loader_ptr = std::make_unique<PackageLoader>();
    module_loader_ptr->SetLogger(logger_ptr_);
    module_loader_ptr->LoadPkg(pkg_options.path, pkg_options.disable_modules, pkg_options.enable_modules);

    MW_TRACE("Load pkg succeeded.\ncfg path: {}\nfull path: {}\nload modules: [{}]",
                pkg_options.path,
                module_loader_ptr->GetDynamicLib().GetLibFullPath(),
                ibmw::common::util::JoinVec(module_loader_ptr->GetLoadedModuleNameList(), ","));

    pkg_options.path = module_loader_ptr->GetDynamicLib().GetLibFullPath();

    package_loader_map_.emplace(pkg_options.path, std::move(module_loader_ptr));
  }

  // Initialize the module directly registered
  for (const auto& item : registered_module_vec_) {
    const auto* module_ptr = item.second;

    MW_REQUIRE(module_ptr != nullptr, "Module point is null!");

    auto info = module_ptr->info(module_ptr->ctx);
    const std::string& module_name = ibmw::util::ToStdString(info.module_name);

    auto module_options = GetModuleOptions(module_name);
    if (module_options && !module_options->enable) {
      continue;
    }

    // Check duplicate modules
    auto finditr = module_wrapper_map_.find(module_name);
    MW_REQUIRE(finditr == module_wrapper_map_.end(),
                            "Duplicate module '{}' in core and lib {}.",
                            module_name, finditr->second->info.pkg_path);

    // Initialize module wrapper
    auto module_wrapper_ptr = std::make_unique<ModuleWrapper>(
        ModuleWrapper{
            .info = util::ModuleDetailInfo{
                .module_name = module_name,
                .pkg_path = item.first,
                .major_version = info.major_version,
                .minor_version = info.minor_version,
                .patch_version = info.patch_version,
                .build_version = info.build_version,
                .author = ibmw::util::ToStdString(info.author),
                .description = ibmw::util::ToStdString(info.description)},
            .loader_ptr = nullptr,
            .module_ptr = module_ptr,
            .core_access_ptr = std::make_unique<CoreAccess>(info)});

    InitModule(module_wrapper_ptr.get());

    module_init_order_.emplace_back(module_name);
    module_detail_info_vec_.emplace_back(&(module_wrapper_ptr->info));
    module_wrapper_map_.emplace(module_name, std::move(module_wrapper_ptr));
  }

  // Initialize the module loaded by dynamic library
  for (const auto& module_loader_itr : package_loader_map_) {
    const auto& pkg_path = module_loader_itr.first;
    PackageLoader& module_loader = *(module_loader_itr.second);

    const auto& module_name_list = module_loader.GetLoadedModuleNameList();
    for (const auto& module_name : module_name_list) {
      auto module_options = GetModuleOptions(module_name);
      if (module_options && !module_options->enable) {
        continue;
      }

      // Check duplicate modules
      MW_REQUIRE(
          module_wrapper_map_.find(module_name) == module_wrapper_map_.end(),
          "Duplicate module '{}' in lib {} and {}.", module_name,
          module_wrapper_map_.find(module_name)->second->info.pkg_path,
          pkg_path);

      // Initialize module wrapper
      const auto* module_ptr = module_loader.GetModule(module_name);

      MW_REQUIRE(module_ptr != nullptr, "Module point is null!");

      auto info = module_ptr->info(module_ptr->ctx);

      auto module_wrapper_ptr = std::make_unique<ModuleWrapper>(
          ModuleWrapper{
              .info = util::ModuleDetailInfo{
                  .module_name = module_name,
                  .pkg_path = pkg_path,
                  .major_version = info.major_version,
                  .minor_version = info.minor_version,
                  .patch_version = info.patch_version,
                  .build_version = info.build_version,
                  .author = ibmw::util::ToStdString(info.author),
                  .description = ibmw::util::ToStdString(info.description)},
              .loader_ptr = &module_loader,
              .module_ptr = module_ptr,
              .core_access_ptr = std::make_unique<CoreAccess>(info)});

      InitModule(module_wrapper_ptr.get());

      module_init_order_.emplace_back(module_name);
      module_detail_info_vec_.emplace_back(&(module_wrapper_ptr->info));
      module_wrapper_map_.emplace(module_name, std::move(module_wrapper_ptr));
    }
  }

  options_node = options_;

  MW_INFO("[Modules] initialized");
}

void ModuleBus::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  for (const auto& module_name : module_init_order_) {
    auto module_wrapper_map_itr = module_wrapper_map_.find(module_name);

    MW_REQUIRE(module_wrapper_map_itr != module_wrapper_map_.end(),
                            "Can not find module '{}'.", module_name);

    const auto* module_ptr = module_wrapper_map_itr->second->module_ptr;

    bool start_ret = module_ptr->run(module_ptr->ctx);
    MW_REQUIRE(start_ret, "Start module '{}' failed.", module_name);

    MW_TRACE("Start module '{}' succeeded.", module_name);
  }

  MW_INFO("[Modules] started");
}

void ModuleBus::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Modules] stopped");

  // Execute Shutdown in reverse order
  for (auto itr = module_init_order_.rbegin(); itr != module_init_order_.rend(); ++itr) {
    const auto& module_name = *itr;
    auto find_itr = module_wrapper_map_.find(module_name);

    if (find_itr == module_wrapper_map_.end()) [[unlikely]] {
      MW_ERROR("Can not find module '{}'.", module_name);
      continue;
    }

    MW_TRACE("Start shutdown module '{}'.", module_name);
    find_itr->second->module_ptr->stop(find_itr->second->module_ptr->ctx);
    MW_TRACE("Shutdown module '{}'.", module_name);
  }

  module_wrapper_map_.clear();

  // package_loader_map_ cannot be cleaned up, many callbacks are registered from other dlls

  module_init_order_.clear();

  registered_module_vec_.clear();

  module_access_configurator_ = CoreAccessConfigurator();
}

void ModuleBus::BroadcastDriverSlot(ibmw_driver_slot_t slot_id,
                                    void* driver_ptr) {
  // (1) Pkg-mode: forward to each loaded dynamic package via typed-slot
  //     dispatch through the pkg ABI.
  for (auto& itr : package_loader_map_) {
    itr.second->BroadcastDriverSlot(slot_id, driver_ptr);
  }

  // (2) Standalone-mode: directly-registered modules expose their
  //     ibmw_module_base_t whose `ctx` field is the Module* itself
  //     (set by Module::GenBase). Call the C++ SetDriverSlot() override
  //     directly via the typed slot id — no pkg ABI roundtrip needed
  //     because we already hold the typed pointer.
  for (auto& [pkg, module_base] : registered_module_vec_) {
    (void)pkg;
    if (module_base != nullptr && module_base->ctx != nullptr) {
      auto* mod_cpp = static_cast<ibmw::Module*>(module_base->ctx);
      mod_cpp->SetDriverSlot(slot_id, driver_ptr);
    }
  }
}

void ModuleBus::RegisterModule(
    std::string_view pkg, const ibmw_module_base_t* module) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  MW_REQUIRE(module != nullptr, "Register invalid module");

  registered_module_vec_.emplace_back(pkg, module);
}

const ibmw_core_base_t* ModuleBus::CreateModule(
    std::string_view pkg, ibmw_module_info_t module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  const std::string& module_name = ibmw::util::ToStdString(module_info.module_name);

  // Manually create module, enable switch does not work

  // Check duplicate modules
  auto finditr = module_wrapper_map_.find(module_name);
  MW_REQUIRE(finditr == module_wrapper_map_.end(),
                          "Duplicate module '{}' in core and lib {}.",
                          module_name, finditr->second->info.pkg_path);

  // Initialize module wrapper
  auto module_wrapper_ptr = std::make_unique<ModuleWrapper>(
      ModuleWrapper{
          .info = util::ModuleDetailInfo{
              .module_name = module_name,
              .pkg_path = std::string(pkg),
              .major_version = module_info.major_version,
              .minor_version = module_info.minor_version,
              .patch_version = module_info.patch_version,
              .build_version = module_info.build_version,
              .author = ibmw::util::ToStdString(module_info.author),
              .description = ibmw::util::ToStdString(module_info.description)},
          .loader_ptr = nullptr,
          .module_ptr = nullptr,
          .core_access_ptr = std::make_unique<CoreAccess>(module_info)});

  const auto* core_base_ptr = module_wrapper_ptr->core_access_ptr->NativeHandle();

  InitModule(module_wrapper_ptr.get());

  module_detail_info_vec_.emplace_back(&(module_wrapper_ptr->info));
  module_wrapper_map_.emplace(module_name, std::move(module_wrapper_ptr));

  return core_base_ptr;
}

void ModuleBus::RegisterCoreAccessConfigurator(
    CoreAccessConfigurator&& module_access_configurator) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  module_access_configurator_ = std::move(module_access_configurator);
}

const std::vector<std::string>& ModuleBus::GetModuleNameList() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return module_init_order_;
}

const std::vector<const util::ModuleDetailInfo*>& ModuleBus::GetModuleDetailInfoList() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return module_detail_info_vec_;
}

std::optional<ModuleBus::Options::ModuleOptions> ModuleBus::GetModuleOptions(std::string_view module_name) {
  auto find_module_options_itr = std::find_if(
      options_.modules_options.begin(), options_.modules_options.end(),
      [&module_name](const auto& module_options) {
        return module_options.name == module_name;
      });

  if (find_module_options_itr != options_.modules_options.end())
    return std::make_optional(*find_module_options_itr);

  return {};
}

void ModuleBus::InitModule(ModuleWrapper* module_wrapper_ptr) {
  const auto& module_name = module_wrapper_ptr->info.module_name;
  const auto* module_ptr = module_wrapper_ptr->module_ptr;

  auto module_options = GetModuleOptions(module_name);
  auto& detail_info = module_wrapper_ptr->info;
  if (module_options) {
    detail_info.log_lvl = module_options->log_lvl;
    detail_info.use_default_log_lvl = module_options->use_default_log_lvl;
    detail_info.cfg_file_path = module_options->cfg_file_path;
  } else {
    options_.modules_options.emplace_back(Options::ModuleOptions{
        .name = module_name,
        .log_lvl = detail_info.log_lvl,
        .use_default_log_lvl = detail_info.use_default_log_lvl,
        .cfg_file_path = detail_info.cfg_file_path});
  }

  module_access_configurator_(detail_info, *(module_wrapper_ptr->core_access_ptr));

  // Initialize the module
  if (module_ptr != nullptr) {
    bool ret = module_ptr->setup(
        module_ptr->ctx, module_wrapper_ptr->core_access_ptr->NativeHandle());

    MW_REQUIRE(ret, "Init module '{}' failed.", module_name);
  }

  MW_TRACE("Init module '{}' succeeded.", module_name);
}

std::list<std::pair<std::string, std::string>> ModuleBus::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  std::list<std::pair<std::string, std::string>> report;

  std::string module_summary;
  if (module_detail_info_vec_.empty()) {
    module_summary = "  (none)\n";
  } else {
    for (const auto& item : module_detail_info_vec_) {
      module_summary += "  " + item->module_name + " v" +
          std::to_string(item->major_version) + "." +
          std::to_string(item->minor_version) + "." +
          std::to_string(item->patch_version) + "." +
          std::to_string(item->build_version);
      if (!item->pkg_path.empty()) {
        module_summary += "  (" + item->pkg_path + ")";
      }
      module_summary += "\n";
    }
  }

  report.emplace_back(
      std::pair<std::string, std::string>{"Modules", module_summary});

  std::vector<std::string> unused_module_options_vec;
  for (const auto& item : options_.modules_options) {
    auto finditr = std::find_if(
        module_detail_info_vec_.begin(), module_detail_info_vec_.end(),
        [name = item.name](const util::ModuleDetailInfo* ptr) {
          return name == ptr->module_name;
        });

    if (finditr == module_detail_info_vec_.end())
      unused_module_options_vec.emplace_back(item.name);
  }

  if (!unused_module_options_vec.empty()) {
    report.emplace_back(
        std::pair<std::string, std::string>{
            "Unused Module Options Warning",
            "  " + ibmw::common::util::JoinVec(unused_module_options_vec, ", ") + "\n"});
  }

  return report;
}

}  // namespace ibmw::runtime::core::module
