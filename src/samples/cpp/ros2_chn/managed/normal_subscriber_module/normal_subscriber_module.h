// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

#include "example_ros2/msg/ros_test_msg.hpp"

namespace ibmw::examples::cpp::ros2_chn::normal_subscriber_module {

class NormalSubscriberModule : public ibmw::Module {
 public:
  NormalSubscriberModule() = default;
  ~NormalSubscriberModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "NormalSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  void EventHandle(const std::shared_ptr<const example_ros2::msg::RosTestMsg>& data);

 private:
  ibmw::Runtime core_;

  std::string topic_name_ = "test_topic";
  ibmw::transport::ReceiverRef subscriber_;
};

}  // namespace ibmw::examples::cpp::ros2_chn::normal_subscriber_module
