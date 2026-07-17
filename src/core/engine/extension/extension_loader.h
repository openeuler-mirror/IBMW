// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "extension_api/ibmw_extension_base.h"
#include "extension_api/ibmw_extension_main.h"
#include "engine/dynamic_lib.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::extension {

class ExtensionLoader {
 public:
  ExtensionLoader()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~ExtensionLoader() {
    // Intentionally do NOT call UnLoadExtension() here.
    // Extension .so files may contain static objects whose destructors reference
    // resources already freed by earlier shutdown phases. Calling dlclose()
    // triggers those destructors and causes "double free" crashes.
    // The OS reclaims all resources when the process exits.
    dynamic_lib_.LeakHandle();
  }

  ExtensionLoader(const ExtensionLoader&) = delete;
  ExtensionLoader& operator=(const ExtensionLoader&) = delete;

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void LoadExtension(std::string_view ext_path);

  void UnLoadExtension();

  EngineExtensionBase* GetExtension() { return extension_ptr_; }

  auto& GetDynamicLib() { return dynamic_lib_; }

 private:
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  std::string ext_path_;
  ibmw::common::util::DynamicLib dynamic_lib_;

  using DestroyFunc = void (*)(const ibmw::EngineExtensionBase*);
  DestroyFunc destroy_func_ = nullptr;

  EngineExtensionBase* extension_ptr_ = nullptr;
};

}  // namespace ibmw::runtime::core::extension
