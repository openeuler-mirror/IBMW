// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/extension/extension_loader.h"
#include "extension_api/ibmw_extension_main.h"

namespace ibmw::runtime::core::extension {

using ExtEntryFunc = ibmw_ext_entry_t (*)();
using ExtEntryV2Func = ibmw_ext_entry_v2_t (*)();

static constexpr const char* kExtEntryFuncName = "IbmwExtEntry";
static constexpr const char* kExtEntryV2FuncName = "IbmwExtEntryV2";

void ExtensionLoader::LoadExtension(std::string_view ext_path) {
  MW_REQUIRE(!dynamic_lib_.IsLoaded() && extension_ptr_ == nullptr,
             "Extension loader already owns lib {}; unload it before loading {}.",
             ext_path_, ext_path);

  ext_path_ = ext_path;

  MW_REQUIRE(dynamic_lib_.Load(ext_path_),
                          "Load dynamic lib failed, lib path {}, error info {}",
                          ext_path_, ibmw::common::util::DynamicLib::GetErr());

  try {
    auto* entry_v2_sym = dynamic_lib_.GetSymbol(kExtEntryV2FuncName);
    if (entry_v2_sym != nullptr) {
      const auto entry = reinterpret_cast<ExtEntryV2Func>(entry_v2_sym)();
      MW_REQUIRE(entry.abi_version == kIbmwExtensionAbiVersionV2,
                 "Extension ABI version mismatch in lib {}: expected {}, got {}.",
                 ext_path_, kIbmwExtensionAbiVersionV2, entry.abi_version);
      MW_REQUIRE(entry.struct_size == sizeof(ibmw_ext_entry_v2_t),
                 "Extension ABI descriptor size mismatch in lib {}: expected {}, got {}.",
                 ext_path_, sizeof(ibmw_ext_entry_v2_t), entry.struct_size);
      MW_REQUIRE(entry.create != nullptr,
                 "Extension V2 entry has null create function in lib {}.", ext_path_);
      MW_REQUIRE(entry.destroy != nullptr,
                 "Extension V2 entry has null destroy function in lib {}.", ext_path_);
      destroy_func_ = entry.destroy;
      extension_ptr_ = entry.create();
    } else {
      auto* entry_sym = dynamic_lib_.GetSymbol(kExtEntryFuncName);
      MW_REQUIRE(entry_sym != nullptr,
                 "Cannot find symbols '{}' or '{}' in lib {}.",
                 kExtEntryV2FuncName, kExtEntryFuncName, ext_path_);

      const auto entry = reinterpret_cast<ExtEntryFunc>(entry_sym)();
      MW_REQUIRE(entry.create != nullptr,
                 "Extension entry has null create function in lib {}.", ext_path_);
      MW_REQUIRE(entry.destroy != nullptr,
                 "Extension entry has null destroy function in lib {}.", ext_path_);
      destroy_func_ = entry.destroy;
      extension_ptr_ = entry.create();
    }

    MW_REQUIRE(extension_ptr_ != nullptr,
               "Cannot create extension in lib {}.", ext_path_);
  } catch (...) {
    UnLoadExtension();
    throw;
  }
}

void ExtensionLoader::UnLoadExtension() {
  if (!dynamic_lib_.IsLoaded()) return;

  if (extension_ptr_ != nullptr) {
    if (destroy_func_ != nullptr) {
      try {
        destroy_func_(extension_ptr_);
        extension_ptr_ = nullptr;
      } catch (...) {
        MW_FATAL("Destroy extension threw while unloading lib {}; the DSO "
                 "will remain mapped.",
                 ext_path_);
        extension_ptr_ = nullptr;
        destroy_func_ = nullptr;
        ext_path_.clear();
        dynamic_lib_.LeakHandle();
        return;
      }
    } else {
      MW_FATAL("Destroy func is null while unloading lib {}; the DSO will "
               "remain mapped.",
               ext_path_);
      extension_ptr_ = nullptr;
      ext_path_.clear();
      dynamic_lib_.LeakHandle();
      return;
    }
  }

  extension_ptr_ = nullptr;
  destroy_func_ = nullptr;
  dynamic_lib_.Unload();
  ext_path_.clear();
}

}  // namespace ibmw::runtime::core::extension
