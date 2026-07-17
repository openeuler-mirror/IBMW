// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

namespace ibmw::examples::cpp::helloworld::helloworld_module {

class HelloWorldModule : public ibmw::Module {
 public:
  HelloWorldModule() = default;
  ~HelloWorldModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "HelloWorldModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

 private:
  ibmw::Runtime core_;
};

}  // namespace ibmw::examples::cpp::helloworld::helloworld_module
