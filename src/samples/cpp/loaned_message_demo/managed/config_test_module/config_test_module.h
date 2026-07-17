// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#pragma once

#include "cpp/module_base.h"
#include "internal/typesupport/ros2/ros2_transport.h"
// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/loaned_message_impl.h"
// IBMW_R1_EXEMPT_END
#include "geometry_msgs/msg/point.hpp"
#include "std_msgs/msg/string.hpp"

namespace ibmw::examples::loaned_message_demo {

class ConfigTestModule : public ibmw::Module {
 public:
  ConfigTestModule() = default;
  ~ConfigTestModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ConfigTestModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  ibmw::logger::LoggerRef GetLogger() const { return core_.GetLogger(); }
  bool Run() override;
  void Stop() override;

  void SetDdsDriver(ibmw::extensions::dds_extension::DdsTransportDriver* driver) {
    driver_ = driver;
  }

 private:
  bool TestAutoModePlain();
  bool TestAutoModeNonPlain();
  bool TestStrictModePlain();
  bool TestStrictModeNonPlain();
  bool TestCompatModePlain();
  bool TestCompatModeNonPlain();

  void PrintResult(const std::string& name, bool passed);
  void PrintSummary();

  ibmw::Runtime core_;
  ibmw::extensions::dds_extension::DdsTransportDriver* driver_ = nullptr;

  int tests_passed_ = 0;
  int tests_failed_ = 0;
};

}  // namespace ibmw::examples::loaned_message_demo
