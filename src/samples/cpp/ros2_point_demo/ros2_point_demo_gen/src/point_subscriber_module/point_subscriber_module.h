// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// IBMW: NOT TRANSPORT-DECOUPLED — direct ros2 API.
// R10 escape-hatch template (direct_ros2_subscriber):
// generated code uses ibmw::ros2::transport API directly for ROS2 DDS
// wire-compatibility. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `subscriber` /
// `loaned_subscriber` templates with ReceiverAccess<T>/LoanedSubscriber<T>.

#pragma once

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
#include <geometry_msgs/msg/point.hpp>

namespace ros2_point_demo {

class PointSubscriberModule : public ibmw::Module {
 public:
  PointSubscriberModule() = default;
  ~PointSubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PointSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override { return true; }
  void Stop() override;

 private:
  void OnMessage(
      ibmw::transport::ContextRef ctx,
      const std::shared_ptr<const geometry_msgs::msg::Point>& msg);

  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  uint32_t receive_count_{0};
};

}  // namespace ros2_point_demo
