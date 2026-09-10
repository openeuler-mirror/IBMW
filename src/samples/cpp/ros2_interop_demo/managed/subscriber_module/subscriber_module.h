// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"
#include <std_msgs/msg/string.hpp>

namespace ibmw::examples::ros2_interop_demo {

class SubscriberModule : public ibmw::Module {
public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "SubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override { return true; }
  void Stop() override;

  ibmw::logger::LoggerRef GetLogger() { return core_.GetLogger(); }

private:
  void OnMessage(
    ibmw::transport::ContextRef ctx,
    const std::shared_ptr<const std_msgs::msg::String>& msg);

  ibmw::Runtime core_;
  uint32_t receive_count_{0};
};

}  // namespace ibmw::examples::ros2_interop_demo
