// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// ROS2 Pub/Sub demo — Subscriber module.

#pragma once

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"

namespace my_project {

class Ros2SubscriberModule : public ibmw::Module {
 public:
  Ros2SubscriberModule() = default;
  ~Ros2SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "Ros2SubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
};

}  // namespace my_project
