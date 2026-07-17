// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// ROS2 Pub/Sub demo — Publisher module.

#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"

#include "example_ros2/msg/ros_test_msg.hpp"

namespace my_project {

class Ros2PublisherModule : public ibmw::Module {
 public:
  Ros2PublisherModule() = default;
  ~Ros2PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "Ros2PublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  // SenderAccess<T> bundles publisher + type info into one object.
  std::optional<ibmw::transport::SenderAccess<example_ros2::msg::RosTestMsg>> access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{false};
};

}  // namespace my_project
