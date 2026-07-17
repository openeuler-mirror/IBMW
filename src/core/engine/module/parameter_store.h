// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "engine/isubsystem.h"
#include "engine/module/parameter_handle.h"
#include "engine/module/parameter_handle_access.h"
#include "engine/core_types.h"
#include "base/string_util.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::parameter {

class ParameterStore : public ISubsystem {
 public:
  struct Options {};

 public:
  ParameterStore() = default;
  ~ParameterStore() override = default;

  ParameterStore(const ParameterStore&) = delete;
  ParameterStore& operator=(const ParameterStore&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  const ParameterHandleAccess& GetParamsAccess(const util::ModuleDetailInfo& module_info);
  const ParameterHandleAccess& GetParamsAccess(std::string_view module_name = "core") {
    return GetParamsAccess(
        util::ModuleDetailInfo{.module_name = std::string(module_name), .pkg_path = "core"});
  }

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

  ParameterHandle* GetParams(std::string_view module_name) const;

 private:
  Options options_;

  class ParameterHandleAccessWrap {
   public:
    ParameterHandleAccessWrap()
        : parameter_handle_access(parameter_handle) {}

    ParameterHandle parameter_handle;
    ParameterHandleAccess parameter_handle_access;
  };

  std::unordered_map<
      std::string,
      std::unique_ptr<ParameterHandleAccessWrap>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      parameter_handle_access_wrap_map_;
};

}  // namespace ibmw::runtime::core::parameter
