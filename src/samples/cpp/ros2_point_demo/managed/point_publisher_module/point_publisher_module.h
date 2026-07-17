// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
#include "geometry_msgs/msg/point.hpp"

namespace ibmw::examples::ros2_point_demo {

class PointPublisherModule : public ibmw::Module {
 public:
  PointPublisherModule() = default;
  ~PointPublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "PointPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  ibmw::logger::LoggerRef GetLogger() const { return core_.GetLogger(); }
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;
  ibmw::transport::SenderRef publisher_;

  std::thread publish_thread_;
  std::atomic<bool> running_{false};
  uint64_t publish_count_{0};
};

}  // namespace ibmw::examples::ros2_point_demo
