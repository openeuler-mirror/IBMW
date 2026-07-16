// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "helloworld_module/helloworld_module.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::helloworld::helloworld_module {

bool HelloWorldModule::Setup(ibmw::Runtime core) {
  // Save ibmw framework handle
  core_ = core;

  // Read cfg
  auto file_path = core_.GetConfigurator().GetConfigFilePath();
  if (!file_path.empty()) {
    YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
    YAML::Node module_cfg_node = cfg_node["HelloWorldModule"] ? cfg_node["HelloWorldModule"] : cfg_node;
    for (const auto& itr : module_cfg_node) {
      if (!itr.second.IsScalar()) {
        continue;
      }
      std::string k = itr.first.as<std::string>();
      std::string v = itr.second.as<std::string>();
      MW_INFO("cfg [{} : {}]", k, v);
    }
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool HelloWorldModule::Run() {
  MW_INFO("Run succeeded.");

  return true;
}

void HelloWorldModule::Stop() {
  MW_INFO("Stop succeeded.");
}

}  // namespace ibmw::examples::cpp::helloworld::helloworld_module
