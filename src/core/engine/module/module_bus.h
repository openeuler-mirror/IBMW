// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "c/logger/logger_base.h"
#include "c/driver_slot.h"
#include "cpp/util/string.h"
#include "engine/isubsystem.h"
#include "engine/module/core_access.h"
#include "engine/module/package_loader.h"
#include "engine/core_types.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::module {

class ModuleBus : public ISubsystem {
 public:
  struct Options {
    struct PkgLoaderOptions {
      std::string path;
      std::vector<std::string> disable_modules;
      std::vector<std::string> enable_modules;
    };
    std::vector<PkgLoaderOptions> pkgs_options;

    struct ModuleOptions {
      std::string name;
      bool enable = true;
      ibmw_log_level_t log_lvl = ibmw_log_level_t::IBMW_LOG_LEVEL_TRACE;
      bool use_default_log_lvl = true;
      std::string cfg_file_path;
    };
    std::vector<ModuleOptions> modules_options;
  };

  using CoreAccessConfigurator = std::function<void(const util::ModuleDetailInfo&, CoreAccess&)>;

 public:
  ModuleBus() = default;
  ~ModuleBus() override = default;

  ModuleBus(const ModuleBus&) = delete;
  ModuleBus& operator=(const ModuleBus&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void RegisterModule(std::string_view pkg, const ibmw_module_base_t* module);
  void RegisterModule(const ibmw_module_base_t* module) { RegisterModule("core", module); }

  const ibmw_core_base_t* CreateModule(std::string_view pkg, ibmw_module_info_t module_info);
  const ibmw_core_base_t* CreateModule(ibmw_module_info_t module_info) {
    return CreateModule("core", module_info);
  }
  const ibmw_core_base_t* CreateModule(std::string_view pkg, std::string_view module_name) {
    return CreateModule(pkg, ibmw_module_info_t{.module_name = ibmw::util::ToIbmwStringView(module_name)});
  }
  const ibmw_core_base_t* CreateModule(std::string_view module_name) {
    return CreateModule(ibmw_module_info_t{.module_name = ibmw::util::ToIbmwStringView(module_name)});
  }

  void RegisterCoreAccessConfigurator(CoreAccessConfigurator&& module_access_configurator);

  // Information query interface
  const std::vector<std::string>& GetModuleNameList() const;
  const std::vector<const util::ModuleDetailInfo*>& GetModuleDetailInfoList() const;

  /**
   * @brief Broadcast a transport driver pointer to every module known to
   *        this bus. Targets BOTH:
   *        (1) modules loaded from a dynamic package (pkg mode), and
   *        (2) directly-registered modules (standalone mode, registered
   *            via RegisterModule()).
   *
   *        Used by Engine after Initialize() and before Start() so that
   *        any module needing direct driver access for zero-copy loan
   *        APIs (e.g. iceoryx EnsurePodTopic, dds create_publisher_with
   *        _driver_config) can opt in via the IBMW_USE_DRIVER /
   *        IBMW_USE_DRIVERS macros and receive a typed pointer without
   *        per-app boilerplate.
   *
   *        Modules that don't opt in see no effect (the base-class
   *        SetDriverSlot no-op runs).
   */
  void BroadcastDriverSlot(ibmw_driver_slot_t slot_id, void* driver_ptr);

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

 private:
  struct ModuleWrapper {
    util::ModuleDetailInfo info;                // Module configuration
    PackageLoader* loader_ptr = nullptr;         // Pointer to the parent dynamic library
    const ibmw_module_base_t* module_ptr;      // Module pointer
    std::unique_ptr<CoreAccess> core_access_ptr;  // The ibmw handle provided to the module
  };

  std::optional<Options::ModuleOptions> GetModuleOptions(std::string_view module_name);
  void InitModule(ModuleWrapper* module_wrapper_ptr);

 private:
  Options options_;

  CoreAccessConfigurator module_access_configurator_;

  // Directly registered module (pkg-module)
  std::vector<std::pair<std::string, const ibmw_module_base_t*>> registered_module_vec_;

  // Module initialization order
  std::vector<std::string> module_init_order_;

  // Dynamic library path-dynamic library
  std::unordered_map<std::string, std::unique_ptr<PackageLoader>> package_loader_map_;

  // Module name - Module
  std::unordered_map<std::string, std::unique_ptr<ModuleWrapper>> module_wrapper_map_;

  // Information query class variables
  std::vector<const util::ModuleDetailInfo*> module_detail_info_vec_;
};

}  // namespace ibmw::runtime::core::module
