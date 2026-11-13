// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "c/driver_slot.h"
#include "c/module_base.h"
#include "pkg/pkg_main.h"
#include "engine/dynamic_lib.h"
#include "base/log_util.h"
#include "base/string_util.h"

namespace ibmw::runtime::core::module {

class PackageLoader {
 public:
  PackageLoader()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~PackageLoader() {
    // Intentionally do NOT unload the module .so files.
    // Module .so files may contain static objects (e.g. ROS2 type support,
    // FastDDS TypeSupport) whose destructors reference resources already freed
    // by earlier shutdown phases (rclcpp context, FastDDS participants, etc.).
    // Calling dlclose() triggers those destructors and causes "double free"
    // crashes. The OS reclaims all resources when the process exits.
    //
    // We leak the DynamicLib handle intentionally to prevent dlclose.
    dynamic_lib_.LeakHandle();
  }

  PackageLoader(const PackageLoader&) = delete;
  PackageLoader& operator=(const PackageLoader&) = delete;

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void LoadPkg(std::string_view pkg_path,
               const std::vector<std::string>& disable_modules,
               const std::vector<std::string>& enable_modules);

  void UnLoadPkg();

  const std::vector<std::string>& GetModuleNameList() const {
    return module_name_vec_;
  }

  const std::vector<std::string>& GetLoadedModuleNameList() const {
    return loaded_module_name_vec_;
  }

  const ibmw_module_base_t* GetModule(std::string_view module_name);
  void DestroyModule(const ibmw_module_base_t* module_ptr);

  /**
   * @brief Broadcast a transport driver pointer to every loaded module
   *        in this package via the typed-slot dispatch path. Silently
   *        skips packages whose pkg entry has no `set_driver_slot`
   *        callback (i.e. modules that need no driver injection).
   *
   * @param slot_id    Stable 64-bit driver slot id (FNV-1a of stable id).
   * @param driver_ptr Type-erased driver pointer.
   */
  void BroadcastDriverSlot(ibmw_driver_slot_t slot_id, void* driver_ptr);

  auto& GetDynamicLib() { return dynamic_lib_; }

 private:
  using CreateModuleFunc = const ibmw_module_base_t* (*)(ibmw_string_view_t);
  void CreateModules(CreateModuleFunc create_module_fn,
                     const std::vector<std::string>& disable_modules,
                     const std::vector<std::string>& enable_modules);

  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  std::string pkg_path_;
  ibmw::common::util::DynamicLib dynamic_lib_;

  using DestroyModuleFunc = void (*)(const ibmw_module_base_t*);
  DestroyModuleFunc destroy_func_ = nullptr;

  using SetDriverSlotFunc = void (*)(ibmw_string_view_t module_name,
                                     ibmw_driver_slot_t slot_id,
                                     void* driver_ptr);
  SetDriverSlotFunc set_driver_slot_func_ = nullptr;

  std::vector<std::string> module_name_vec_;
  std::vector<std::string> loaded_module_name_vec_;
  std::unordered_map<
      std::string,
      const ibmw_module_base_t*,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      module_ptr_map_;

  // Create an enumeration class to identify the usage of the pkgs module (UseNone: use all, UseEnable: use enabled module, UseDisable: use disabled module, use UseNone by default)
  enum class EnableOrDisable : uint32_t {
    kUseNone,
    kUseEnable,
    kUseDisable,
  } enable_or_disable_for_pkg_ = EnableOrDisable::kUseNone;
};

}  // namespace ibmw::runtime::core::module
