// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

#include "c/configurator/configurator_base.h"
#include "cpp/util/string.h"
#include "base/exception.h"

namespace ibmw::configurator {

class ConfiguratorRef {
 public:
  ConfiguratorRef() = default;
  explicit ConfiguratorRef(const ibmw_configurator_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ConfiguratorRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_configurator_base_t* NativeHandle() const { return base_ptr_; }

  /**
   * @brief Get the config file path
   *
   * @return Config file path
   */
  std::string_view GetConfigFilePath() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return ibmw::util::ToStdStringView(base_ptr_->config_file_path(base_ptr_->ctx));
  }

 private:
  const ibmw_configurator_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::configurator
