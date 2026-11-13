// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/module/package_loader.h"
#include "cpp/util/string.h"
#include "pkg/pkg_main.h"

namespace ibmw::runtime::core::module {

using PkgEntryFunc = ibmw_pkg_entry_v2_t (*)();

static constexpr const char* kPkgEntryFuncName = "IbmwPkgEntryV2";
// Entry point of the unversioned package ABI. Only looked up to explain the
// rejection; never called.
static constexpr const char* kOldPkgEntryFuncName = "IbmwPkgEntry";

// The name list length is only known from get_module_num(); bounding it keeps
// a broken package from making the loader read far past its array.
static constexpr size_t kMaxModulesPerPkg = 4096;

void PackageLoader::LoadPkg(std::string_view pkg_path,
                           const std::vector<std::string>& disable_modules,
                           const std::vector<std::string>& enable_modules) {
  pkg_path_ = pkg_path;

  MW_REQUIRE(dynamic_lib_.Load(pkg_path_),
                          "Load dynamic lib failed, lib path {}, error info {}",
                          pkg_path_, ibmw::common::util::DynamicLib::GetErr());

  // pkg ABI: typed-slot dispatch via IbmwPkgEntryV2().
  auto* entry_sym = dynamic_lib_.GetSymbol(kPkgEntryFuncName);
  if (entry_sym == nullptr) {
    MW_REQUIRE(dynamic_lib_.GetSymbol(kOldPkgEntryFuncName) == nullptr,
               "Lib {} exports '{}' from an older IBMW package ABI; "
               "rebuild it against this IBMW (package ABI {}).",
               pkg_path_, kOldPkgEntryFuncName, IBMW_PKG_ABI_VERSION_V2);
  }
  MW_REQUIRE(entry_sym != nullptr,
                          "Cannot find symbol '{}' in lib {}.",
                          kPkgEntryFuncName, pkg_path_);

  auto entry = ((PkgEntryFunc)entry_sym)();
  MW_REQUIRE(entry.abi_version == IBMW_PKG_ABI_VERSION_V2 && entry.struct_size >= sizeof(ibmw_pkg_entry_v2_t),
             "Lib {} has package ABI version {} with struct_size {}; expected version {} and size at least {}. "
             "Rebuild it against this IBMW.",
             pkg_path_, entry.abi_version, entry.struct_size, IBMW_PKG_ABI_VERSION_V2, sizeof(ibmw_pkg_entry_v2_t));
  MW_REQUIRE(entry.get_module_num != nullptr && entry.get_module_name_list != nullptr &&
                 entry.create_module != nullptr && entry.destroy_module != nullptr,
                          "pkg ABI entry has null mandatory callbacks in lib {}.", pkg_path_);

  auto* get_module_num_fn = entry.get_module_num;
  auto* get_module_name_list_fn = entry.get_module_name_list;
  auto* create_module_fn = entry.create_module;
  auto* destroy_module_fn = entry.destroy_module;
  set_driver_slot_func_ = entry.set_driver_slot;

  destroy_func_ = destroy_module_fn;

  size_t module_num = get_module_num_fn();
  MW_REQUIRE(module_num > 0, "No module in lib {}.", pkg_path_);
  MW_REQUIRE(module_num <= kMaxModulesPerPkg, "Lib {} reports {} modules, at most {} are supported.",
             pkg_path_, module_num, kMaxModulesPerPkg);

  const ibmw_string_view_t* module_name_array = get_module_name_list_fn();
  MW_REQUIRE(module_name_array != nullptr,
                          "Module name list is null in lib {}.", pkg_path_);

  // Check the module list
  module_name_vec_.reserve(module_num);
  for (size_t ii = 0; ii < module_num; ++ii) {
    auto module_name = ibmw::util::ToStdStringView(module_name_array[ii]);
    MW_REQUIRE(!module_name.empty(),
                            "Module name index [{}] is empty in lib {}.",
                            ii, pkg_path_);

    MW_REQUIRE(
        std::find(module_name_vec_.begin(), module_name_vec_.end(), module_name) == module_name_vec_.end(),
        "Module name '{}' repeated in lib {}.", module_name, pkg_path_);

    module_name_vec_.emplace_back(module_name);
  }

  // Loading module
  loaded_module_name_vec_.reserve(module_num);

  // Before loading the module, check whether the enabled and disabled modules conflict (alarm only once)
  if (!disable_modules.empty() && !enable_modules.empty()) {
    MW_WARN("Enabled modules and disabled modules are conflicted! Only enabled modules are loaded, disabled modules are ignored!\n");
    enable_or_disable_for_pkg_ = EnableOrDisable::kUseEnable;
  } else if (!disable_modules.empty() && enable_modules.empty()) {
    enable_or_disable_for_pkg_ = EnableOrDisable::kUseDisable;
  } else if (disable_modules.empty() && !enable_modules.empty()) {
    enable_or_disable_for_pkg_ = EnableOrDisable::kUseEnable;
  } else {
    enable_or_disable_for_pkg_ = EnableOrDisable::kUseNone;
  }

  try {
    CreateModules(create_module_fn, disable_modules, enable_modules);
  } catch (...) {
    // The modules created so far would leak: nothing refers to them anymore.
    for (auto itr = loaded_module_name_vec_.rbegin(); itr != loaded_module_name_vec_.rend(); ++itr) {
      if (auto found = module_ptr_map_.find(*itr); found != module_ptr_map_.end()) DestroyModule(found->second);
    }
    module_ptr_map_.clear();
    loaded_module_name_vec_.clear();
    throw;
  }
}

void PackageLoader::CreateModules(CreateModuleFunc create_module_fn,
                                  const std::vector<std::string>& disable_modules,
                                  const std::vector<std::string>& enable_modules) {
  for (size_t ii = 0; ii < module_name_vec_.size(); ++ii) {
    const auto& module_name = module_name_vec_[ii];
    auto finditr_disable = std::find(disable_modules.begin(), disable_modules.end(), module_name);
    auto finditr_enable = std::find(enable_modules.begin(), enable_modules.end(), module_name);

    // If enable_or_disable_for_pkg_ selects the disabled module, it will skip the module in the disabled list when it encounters the module in the disabled list.
    if (enable_or_disable_for_pkg_ == EnableOrDisable::kUseDisable && finditr_disable != disable_modules.end()) {
      continue;
    }

    // If enable_or_disable_for_pkg_ selects the enable module, it will skip it if it encounters a module that is not in the enable list.
    if (enable_or_disable_for_pkg_ == EnableOrDisable::kUseEnable && finditr_enable == enable_modules.end()) {
      continue;
    }

    const ibmw_module_base_t* module_ptr =
        create_module_fn(ibmw::util::ToIbmwStringView(module_name));

    MW_REQUIRE(module_ptr != nullptr,
                            "Cannot create module '{}' in lib {}.",
                            module_name, pkg_path_);
    if (module_ptr->info == nullptr) [[unlikely]] {
      DestroyModule(module_ptr);
      MW_THROW("Module '{}' in lib {} has no info callback.", module_name, pkg_path_);
    }

    auto module_info = module_ptr->info(module_ptr->ctx);
    auto real_module_name = ibmw::util::ToStdStringView(module_info.module_name);
    if (real_module_name != module_name) [[unlikely]] {
      const std::string real_name(real_module_name);  // info may die with the module
      DestroyModule(module_ptr);
      MW_THROW("Require module name '{}', but get module name '{}' in lib {}.",
               module_name, real_name, pkg_path_);
    }

    loaded_module_name_vec_.emplace_back(module_name);
    module_ptr_map_.emplace(module_name, module_ptr);
  }
}

void PackageLoader::UnLoadPkg() {
  if (!dynamic_lib_.IsLoaded()) return;

  for (auto& module_ptr_itr : module_ptr_map_)
    DestroyModule(module_ptr_itr.second);

  module_ptr_map_.clear();
  loaded_module_name_vec_.clear();
  module_name_vec_.clear();
  destroy_func_ = nullptr;

  dynamic_lib_.Unload();
}

const ibmw_module_base_t* PackageLoader::GetModule(std::string_view module_name) {
  auto finditr = module_ptr_map_.find(module_name);
  if (finditr == module_ptr_map_.end()) {
    MW_ERROR("No module name '{}' in lib {}", module_name, pkg_path_);
    return nullptr;
  }
  return finditr->second;
}

void PackageLoader::DestroyModule(const ibmw_module_base_t* module_ptr) {
  if (module_ptr == nullptr) {
    MW_WARN("Destroy a null pointer!");
    return;
  }

  if (destroy_func_ == nullptr) {
    MW_WARN("Destroy func is null!");
    return;
  }

  destroy_func_(module_ptr);
}

void PackageLoader::BroadcastDriverSlot(ibmw_driver_slot_t slot_id,
                                        void* driver_ptr) {
  if (set_driver_slot_func_ == nullptr) {
    // Package without driver-injection callback. Silently skip.
    return;
  }
  for (const auto& module_name : loaded_module_name_vec_) {
    set_driver_slot_func_(
        ibmw::util::ToIbmwStringView(module_name),
        slot_id,
        driver_ptr);
  }
}

}  // namespace ibmw::runtime::core::module
