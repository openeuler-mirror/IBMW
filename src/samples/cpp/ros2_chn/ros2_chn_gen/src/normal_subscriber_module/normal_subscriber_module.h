// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "normal_subscriber_module_typesupport.inc.h"

namespace ros2_chn_demo {

class NormalSubscriberModule : public ibmw::Module {
 public:
  NormalSubscriberModule() = default;
  ~NormalSubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "NormalSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
};

}  // namespace ros2_chn_demo
