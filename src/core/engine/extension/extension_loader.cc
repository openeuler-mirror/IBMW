// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/extension/extension_loader.h"
#include "extension_api/ibmw_extension_main.h"

namespace ibmw::runtime::core::extension {

using ExtEntryFunc = ibmw_ext_entry_t (*)();

static constexpr const char* kExtEntryFuncName = "IbmwExtEntry";

void ExtensionLoader::LoadExtension(std::string_view ext_path) {
  ext_path_ = ext_path;

  MW_REQUIRE(dynamic_lib_.Load(ext_path_),
                          "Load dynamic lib failed, lib path {}, error info {}",
                          ext_path_, ibmw::common::util::DynamicLib::GetErr());

  auto* entry_sym = dynamic_lib_.GetSymbol(kExtEntryFuncName);
  MW_REQUIRE(entry_sym != nullptr,
                          "Cannot find symbol '{}' in lib {}.",
                          kExtEntryFuncName, ext_path_);

  auto entry = ((ExtEntryFunc)entry_sym)();

  MW_REQUIRE(entry.create != nullptr,
                          "Extension entry has null create function in lib {}.", ext_path_);
  MW_REQUIRE(entry.destroy != nullptr,
                          "Extension entry has null destroy function in lib {}.", ext_path_);

  destroy_func_ = entry.destroy;
  extension_ptr_ = entry.create();

  MW_REQUIRE(extension_ptr_ != nullptr,
                          "Cannot create extension in lib {}.", ext_path_);
}

void ExtensionLoader::UnLoadExtension() {
  if (!dynamic_lib_.IsLoaded()) return;

  if (extension_ptr_ != nullptr) {
    if (destroy_func_ != nullptr) {
      destroy_func_(extension_ptr_);
    } else {
      MW_WARN("Destroy func is null!");
    }
  }

  destroy_func_ = nullptr;
  dynamic_lib_.Unload();
}

}  // namespace ibmw::runtime::core::extension
