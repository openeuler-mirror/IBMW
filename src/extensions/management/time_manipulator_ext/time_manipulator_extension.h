// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "engine/extension/extension_base.h"
#include "time_manipulator_ext/service.h"

namespace ibmw::extensions::time_manipulator_extension {

class TimeManipulatorExtension : public ExtensionBase {
 public:
  struct Options {
    std::string service_name;
  };

 public:
  TimeManipulatorExtension();
  ~TimeManipulatorExtension() override = default;

  std::string_view Name() const noexcept override { return "time_manipulator_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterTimeManipulatorExecutor();
  void RegisterRpcService();

 private:
  Options options_;

  std::unique_ptr<TimeManipulatorServiceImpl> service_ptr_;
};

}  // namespace ibmw::extensions::time_manipulator_extension
