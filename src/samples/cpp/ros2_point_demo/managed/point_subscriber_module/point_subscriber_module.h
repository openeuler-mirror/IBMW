// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include <atomic>

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
#include "geometry_msgs/msg/point.hpp"

namespace ibmw::examples::ros2_point_demo {

class PointSubscriberModule : public ibmw::Module {
 public:
  PointSubscriberModule() = default;
  ~PointSubscriberModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "PointSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override { return true; }

  ibmw::logger::LoggerRef GetLogger() const { return core_.GetLogger(); }
  void Stop() override;

 private:
  void OnMessage(ibmw::transport::ContextRef ctx,
                 const std::shared_ptr<const geometry_msgs::msg::Point>& msg);

  ibmw::Runtime core_;
  std::atomic<uint64_t> receive_count_{0};
};

}  // namespace ibmw::examples::ros2_point_demo
