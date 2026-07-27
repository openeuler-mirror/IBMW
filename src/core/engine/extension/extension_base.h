// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// ExtensionBase — base class for all IBMW extensions.
//
// Eliminates per-extension boilerplate:
//  - IBMW_EXT_ENTRY(Class) replaces identical *_main.cc files
//  - Automatic logger lifecycle hooks (After(kInitialize, kLogger) / BeforePhase(kStopping))
//  - Thread-safe logger storage via ExtensionLogger helper
//
// Subclasses override:
//  - Name()       — extension identifier (required)
//  - OnInit()     — called during extension initialization (required)
//  - OnShutdown() — called during extension shutdown (optional)
//  - GenInitializationReport() — optional diagnostics

#pragma once

#include <atomic>
#include <list>
#include <string>
#include <string_view>
#include <utility>

#include "extension_api/ibmw_extension_base.h"
#include "extension_api/ibmw_extension_export.h"
#include "extension_api/ibmw_extension_main.h"
#include "cpp/logger/logger.h"

namespace ibmw::runtime::core {
class Engine;
}

namespace ibmw {

// ---------------------------------------------------------------------------
// ExtensionLogger — thread-safe per-extension logger storage.
//
// Each extension places one instance in its translation unit (typically in the
// extension's global.cc or at file scope).  ExtensionBase auto-manages it via
// the After(kInitialize, kLogger) / BeforePhase(kStopping) hooks.
//
// Usage in extension global.h:
//   extern ExtensionLogger g_logger;
//   #define MW_LOGGER ibmw::extensions::foo_extension::g_logger.Get()
//   #include "base/log_util.h"
//
// Usage in extension global.cc:
//   ExtensionLogger g_logger;
// ---------------------------------------------------------------------------
class ExtensionLogger {
 public:
  ExtensionLogger() = default;

  void Set(ibmw::logger::LoggerRef ref) {
    logger_.store(ref, std::memory_order_release);
  }

  ibmw::logger::LoggerRef Get() const {
    auto ref = logger_.load(std::memory_order_acquire);
    return ref ? ref : ibmw::logger::GetSimpleLoggerRef();
  }

 private:
  std::atomic<ibmw::logger::LoggerRef> logger_{};
};

// ---------------------------------------------------------------------------
// ExtensionBase — base class for IBMW extensions.
//
// Manages the logger lifecycle automatically:
//  - After the logging subsystem initializes, the extension receives the
//    real logger via a hook and writes it to the ExtensionLogger.
//  - Before shutdown, the logger is reverted so no dangling pointer occurs.
//
// Subclasses implement OnInit / OnShutdown instead of Initialize / Shutdown.
// ---------------------------------------------------------------------------
class ExtensionBase : public EngineExtensionBase {
 public:
  ExtensionBase() = default;
  ~ExtensionBase() override = default;

  // -- Lifecycle (called by the framework — do NOT override) ----------------

  /// Framework entry point.  Registers logger hooks, then delegates to OnInit.
  bool Initialize(runtime::core::Engine* core_ptr) noexcept final;

  /// Framework shutdown.  Delegates to OnShutdown, then reverts the logger.
  void Shutdown() noexcept final;

  // -- Subclass interface ---------------------------------------------------

  /// Return the unique name of this extension.
  std::string_view Name() const noexcept override = 0;

  /// Called after the base class has stored core_ptr and registered hooks.
  /// Return true on success.
  virtual bool OnInit(runtime::core::Engine* core_ptr) = 0;

  /// Called before logger teardown.  Default does nothing.
  virtual void OnShutdown() {}

  /// Optional: contribute to the startup diagnostics report.
  std::list<std::pair<std::string, std::string>>
  GenInitializationReport() const override { return {}; }

  // -- Logger binding -------------------------------------------------------

  /// Set the extension's logger storage.  Called in the constructor of each
  /// concrete extension, BEFORE Initialize runs.
  void SetExtensionLogger(ExtensionLogger* pl) { extension_logger_ = pl; }

  /// Get the current logger (thread-safe, with simple-logger fallback).
  ibmw::logger::LoggerRef Logger() const {
    return extension_logger_ ? extension_logger_->Get()
                          : ibmw::logger::GetSimpleLoggerRef();
  }

 protected:
  /// The core pointer, valid between Initialize and Shutdown.
  runtime::core::Engine* core_ptr_ = nullptr;

 private:
  ExtensionLogger* extension_logger_ = nullptr;
  bool init_ok_ = false;
};

}  // namespace ibmw

// ---------------------------------------------------------------------------
// IBMW_EXT_ENTRY — declares the single extension entry point.
//
// Place at file scope (NOT inside any namespace) in the extension's .cc file:
//   IBMW_EXT_ENTRY(ibmw::extensions::echo_extension::EchoExtension)
// ---------------------------------------------------------------------------
#define IBMW_EXT_ENTRY(ExtensionClass)                                              \
  extern "C" {                                                                      \
  IBMW_CORE_EXTENSION_EXPORT                                                        \
  ibmw_ext_entry_t IbmwExtEntry() {                                                 \
    return ibmw_ext_entry_t{                                                        \
        .create = []() -> ibmw::EngineExtensionBase* {                              \
          return new ExtensionClass();                                               \
        },                                                                          \
        .destroy = [](const ibmw::EngineExtensionBase* ext) {                       \
          delete ext;                                                               \
        }};                                                                         \
  }                                                                                 \
  } /* extern "C" */

// V2 is intentionally a separate macro so existing extensions remain legacy
// compatible until they explicitly opt in.
#define IBMW_EXT_ENTRY_V2(ExtensionClass)                                           \
  extern "C" {                                                                      \
  IBMW_CORE_EXTENSION_EXPORT                                                        \
  ibmw_ext_entry_v2_t IbmwExtEntryV2() {                                            \
    return ibmw_ext_entry_v2_t{                                                      \
        .abi_version = kIbmwExtensionAbiVersionV2,                                  \
        .struct_size = sizeof(ibmw_ext_entry_v2_t),                                 \
        .create = []() -> ibmw::EngineExtensionBase* {                              \
          return new ExtensionClass();                                               \
        },                                                                          \
        .destroy = [](const ibmw::EngineExtensionBase* ext) {                       \
          delete ext;                                                               \
        }};                                                                         \
  }                                                                                 \
  } /* extern "C" */
