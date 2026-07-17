// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "c/configurator/configurator_base.h"
#include "cpp/util/string.h"

namespace ibmw::runtime::core::configurator {

class ConfiguratorAccess {
 public:
  explicit ConfiguratorAccess(std::string_view config_file_path = "")
      : config_file_path_(config_file_path),
        base_(GenBase(this)) {}
  ~ConfiguratorAccess() = default;

  void SetConfigFilePath(std::string_view path) {
    auto& mutable_path = const_cast<std::string&>(config_file_path_);
    mutable_path = path;
  }

  ConfiguratorAccess(const ConfiguratorAccess&) = delete;
  ConfiguratorAccess& operator=(const ConfiguratorAccess&) = delete;

  const ibmw_configurator_base_t* NativeHandle() const { return &base_; }

 private:
  static ibmw_configurator_base_t GenBase(void* ctx) {
    return ibmw_configurator_base_t{
        .config_file_path = [](void* ctx) -> ibmw_string_view_t {
          auto* access = static_cast<ConfiguratorAccess*>(ctx);
          return ibmw::util::ToIbmwStringView(access->config_file_path_);
        },
        .ctx = ctx};
  }

 private:
  const std::string config_file_path_;
  const ibmw_configurator_base_t base_;
};

}  // namespace ibmw::runtime::core::configurator
