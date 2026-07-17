// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include <atomic>

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
#include "geometry_msgs/msg/point.hpp"
#include "std_msgs/msg/string.hpp"

namespace ibmw::examples::loaned_message_demo {

class SubscriberModule : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "SubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  ibmw::logger::LoggerRef GetLogger() const { return core_.GetLogger(); }
  bool Run() override;
  void Stop() override;

 private:
  void OnPointMessage(const std::shared_ptr<const geometry_msgs::msg::Point>& msg);
  void OnStringMessage(const std::shared_ptr<const std_msgs::msg::String>& msg);

  ibmw::Runtime core_;
  std::atomic<uint64_t> point_count_{0};
  std::atomic<uint64_t> string_count_{0};
};

}  // namespace ibmw::examples::loaned_message_demo
