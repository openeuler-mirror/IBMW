// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "engine/isubsystem.h"
#include "engine/module/configurator_access.h"
#include "engine/core_types.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::configurator {

class ConfigSubsystem : public ISubsystem {
 public:
  struct Options {
    std::filesystem::path temp_cfg_path = "./cfg/tmp";
  };

 public:
  ConfigSubsystem() = default;
  ~ConfigSubsystem() override {
    delete ori_root_options_node_ptr_;
    delete root_options_node_ptr_;
    delete user_root_options_node_ptr_;
  }

  ConfigSubsystem(const ConfigSubsystem&) = delete;
  ConfigSubsystem& operator=(const ConfigSubsystem&) = delete;

  // ConfigSubsystem is special: it takes a filesystem path, not a YAML node.
  // The YAML-based Initialize() satisfies the interface but must not be called.
  void Initialize(YAML::Node /*options_node*/) override {
    MW_REQUIRE(false, "ConfigSubsystem::Initialize(YAML::Node) must not be called; "
                      "use Initialize(const std::filesystem::path&) instead.");
  }

  void Initialize(const std::filesystem::path& cfg_file_path);
  void Start() override;
  void Shutdown() override;

  YAML::Node GetOriRootOptionsNode() const;
  YAML::Node GetRootOptionsNode() const;
  YAML::Node GetUserRootOptionsNode() const;

  std::string GetConfigureFilePath() const { return cfg_file_path_.string(); }

  void SetDefaultConfigFilePath(std::string_view path) {
    default_cfg_access_.SetConfigFilePath(path);
  }

  const ConfiguratorAccess& GetConfiguratorAccess(const util::ModuleDetailInfo& module_info);

  YAML::Node GetIbmwOptionsNode(std::string_view key);

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

 private:
  std::filesystem::path cfg_file_path_;
  Options options_;

  YAML::Node* ori_root_options_node_ptr_ = nullptr;   // Configuration with standardized ibmw nodes and user module nodes
  YAML::Node* root_options_node_ptr_ = nullptr;       // Configuration containing only standardized ibmw nodes
  YAML::Node* user_root_options_node_ptr_ = nullptr;  // Originally loaded configuration

  std::unordered_map<std::string, std::unique_ptr<ConfiguratorAccess>> cfg_access_map_;
  ConfiguratorAccess default_cfg_access_;
};

}  // namespace ibmw::runtime::core::configurator
