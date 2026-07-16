// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/extension/extension_base.h"

#include "engine/engine.h"

namespace ibmw {

bool ExtensionBase::Initialize(runtime::core::Engine* core_ptr) noexcept {
  try {
    core_ptr_ = core_ptr;

    // Auto-register logger lifecycle hooks.
    // After log subsystem init: bind the real logger.
    core_ptr_->RegisterHook(
        runtime::core::After(runtime::core::Phase::kInitialize,
                             runtime::core::Subsystem::kLogger),
        [this] {
          if (extension_logger_) {
            extension_logger_->Set(ibmw::logger::LoggerRef(
                core_ptr_->GetLoggingSubsystem().GetLoggerAccess().NativeHandle()));
          }
        });

    // Before shutdown: revert to simple logger to prevent dangling pointers.
    core_ptr_->RegisterHook(
        runtime::core::BeforePhase(runtime::core::Phase::kStopping),
        [this] {
          if (extension_logger_) {
            extension_logger_->Set(ibmw::logger::LoggerRef{});
          }
        });

    // Delegate to subclass.
    bool ok = OnInit(core_ptr);
    init_ok_ = ok;
    return ok;
  } catch (const std::exception& e) {
    // Use simple logger since the real logger may not be available yet.
    auto lgr = Logger();
    MW_LOG(lgr, ibmw::common::util::kLogLevelError,
           "Extension '{}' Initialize failed: {}", Name(), e.what());
    return false;
  }
}

void ExtensionBase::Shutdown() noexcept {
  try {
    if (!init_ok_) return;
    OnShutdown();
  } catch (const std::exception& e) {
    auto lgr = Logger();
    MW_LOG(lgr, ibmw::common::util::kLogLevelError,
           "Extension '{}' Shutdown failed: {}", Name(), e.what());
  }
}

}  // namespace ibmw
