// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <string>

#include "engine/isubsystem.h"
#include "engine/extension/extension_loader.h"
#include "extension_api/ibmw_extension_runtime.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::extension {

class ExtensionBus : public ISubsystem {
 public:
  struct Options {
    struct ExtensionOptions {
      std::string name;
      std::string path;
      YAML::Node options;
    };
    std::vector<ExtensionOptions> extensions_options;
  };

  using ExtensionInitFunc = std::function<bool(EngineExtensionBase*)>;

 public:
  ExtensionBus() = default;
  ~ExtensionBus() override = default;

  ExtensionBus(const ExtensionBus&) = delete;
  ExtensionBus& operator=(const ExtensionBus&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void RegisterExtension(EngineExtensionBase* extension);

  void RegisterCorePtr(Engine* core_ptr);

  YAML::Node GetExtensionOptionsNode(std::string_view ext_name) const;
  void UpdateExtensionOptionsNode(std::string_view ext_name, YAML::Node options);

  const Options& GetOptions() const { return options_; }

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

  const std::vector<EngineExtensionBase*>& GetUsedExtension() const;

 private:
  Options options_;

  Engine* core_ptr_ = nullptr;
  std::unique_ptr<ibmw::extension::RuntimeV2> runtime_v2_;

  // Register extension directly
  std::vector<EngineExtensionBase*> registered_extension_vec_;

  // Load extensions through dynamic libraries
  std::vector<std::unique_ptr<ExtensionLoader>> extension_loader_vec_;

  std::vector<EngineExtensionBase*> used_extension_vec_;
};
}  // namespace ibmw::runtime::core::extension
