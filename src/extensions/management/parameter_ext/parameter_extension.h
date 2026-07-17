// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "engine/extension/extension_base.h"
#include "parameter_ext/service.h"

namespace ibmw::extensions::parameter_extension {

class ParameterExtension : public ExtensionBase {
 public:
  struct Options {
    std::string service_name;
  };

 public:
  ParameterExtension();
  ~ParameterExtension() override = default;

  std::string_view Name() const noexcept override { return "parameter_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterRpcService();

 private:
  Options options_;

  std::unique_ptr<ParameterServiceImpl> service_ptr_;
};

}  // namespace ibmw::extensions::parameter_extension
