// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"
// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/ros2_style_api.h"
#include "dds_ext/dds_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "geometry_msgs/msg/point.hpp"

namespace ibmw::examples::loaned_message_demo {

class PlainPublisherModule : public ibmw::Module {
 public:
  PlainPublisherModule() = default;
  ~PlainPublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "PlainPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  ibmw::logger::LoggerRef GetLogger() const { return core_.GetLogger(); }
  bool Run() override;
  void Stop() override;

  void SetDdsDriver(ibmw::extensions::dds_extension::DdsTransportDriver* driver) {
    driver_ = driver;
  }

 private:
  void PublishLoop();
  void PrintStatistics();

  ibmw::Runtime core_;
  ibmw::extensions::dds_extension::DdsTransportDriver* driver_ = nullptr;

  using PointPublisher = ibmw::extensions::dds_extension::Ros2StylePublisher<geometry_msgs::msg::Point>;
  std::shared_ptr<PointPublisher> loaned_publisher_;

  std::thread publish_thread_;
  std::atomic<bool> running_{false};
  uint64_t publish_count_{0};
  uint64_t zero_copy_count_{0};
  uint64_t fallback_count_{0};
};

}  // namespace ibmw::examples::loaned_message_demo
