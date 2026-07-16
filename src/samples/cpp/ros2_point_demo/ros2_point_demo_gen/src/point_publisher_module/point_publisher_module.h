// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// IBMW: NOT TRANSPORT-DECOUPLED — direct ros2 API.
// R10 escape-hatch template (direct_ros2_publisher):
// generated code uses ibmw::ros2::transport API directly for ROS2 DDS
// wire-compatibility. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `publisher` /
// `loaned_publisher` templates with SenderAccess<T>/LoanedSample<T>.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
#include <geometry_msgs/msg/point.hpp>

namespace ros2_point_demo {

class PointPublisherModule : public ibmw::Module {
 public:
  PointPublisherModule() = default;
  ~PointPublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PointPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  ibmw::transport::SenderRef publisher_;

  std::thread publish_thread_;
  std::atomic<bool> running_{false};
  uint32_t publish_count_{0};
};

}  // namespace ros2_point_demo
