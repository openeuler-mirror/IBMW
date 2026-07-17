// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/isubsystem.h"
#include "engine/module/allocator_access.h"
#include "engine/core_types.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::allocator {

class MemoryBus : public ISubsystem {
 public:
  struct Options {};

 public:
  MemoryBus() = default;
  ~MemoryBus() override = default;

  MemoryBus(const MemoryBus&) = delete;
  MemoryBus& operator=(const MemoryBus&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  const AllocatorAccess& GetAllocatorAccess(const util::ModuleDetailInfo& module_info);
  const AllocatorAccess& GetAllocatorAccess(std::string_view module_name = "core") {
    return GetAllocatorAccess(
        util::ModuleDetailInfo{.module_name = std::string(module_name), .pkg_path = "core"});
  }

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

 private:
  Options options_;

  AllocatorAccess allocator_access_;
};

}  // namespace ibmw::runtime::core::allocator
