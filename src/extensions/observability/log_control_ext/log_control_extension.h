// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "engine/extension/extension_base.h"
#include "log_control_ext/service.h"

namespace ibmw::extensions::log_control_extension {

class LogControlExtension : public ExtensionBase {
 public:
  struct Options {
    std::string service_name;
  };

 public:
  LogControlExtension();
  ~LogControlExtension() override = default;

  std::string_view Name() const noexcept override { return "log_control_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterRpcService();

 private:
  Options options_;

  std::unique_ptr<LogControlServiceImpl> service_ptr_;
};

}  // namespace ibmw::extensions::log_control_extension
