// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <list>
#include <string>
#include <string_view>

namespace ibmw {

namespace extension {
class RuntimeV2;
}

namespace runtime::core {
class Engine;
}

class EngineExtensionBase {
 public:
  EngineExtensionBase() = default;
  virtual ~EngineExtensionBase() = default;

  EngineExtensionBase(const EngineExtensionBase&) = delete;
  EngineExtensionBase& operator=(const EngineExtensionBase&) = delete;

  virtual std::string_view Name() const noexcept = 0;

  virtual bool Initialize(runtime::core::Engine* core_ptr) noexcept = 0;
  virtual void Shutdown() noexcept = 0;

  // New extensions can opt into the narrow extension-facing runtime contract.
  // Legacy extensions continue to receive Engine until migrated.
  virtual uint32_t RequiredRuntimeAbi() const noexcept { return 0; }
  virtual bool InitializeRuntime(extension::RuntimeV2*) noexcept {
    return false;
  }

  virtual std::list<std::pair<std::string, std::string>> GenInitializationReport() const { return {}; }
};

}  // namespace ibmw
