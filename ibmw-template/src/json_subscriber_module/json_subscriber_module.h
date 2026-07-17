// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// JSON Pub/Sub demo — Subscriber module.

#pragma once

#include "cpp/module_base.h"

namespace my_project {

class JsonSubscriberModule : public ibmw::Module {
 public:
  JsonSubscriberModule() = default;
  ~JsonSubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "JsonSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
};

}  // namespace my_project
