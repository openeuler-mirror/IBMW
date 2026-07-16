// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
#include <std_msgs/msg/string.hpp>

namespace ibmw::examples::ros2_interop_demo {

class PublisherModule : public ibmw::Module {
public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "PublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

  ibmw::logger::LoggerRef GetLogger() { return core_.GetLogger(); }

private:
  void PublishLoop();

  ibmw::Runtime core_;
  ibmw::transport::SenderRef publisher_;

  std::thread publish_thread_;
  std::atomic<bool> running_{false};
  uint32_t publish_count_{0};
};

}  // namespace ibmw::examples::ros2_interop_demo
