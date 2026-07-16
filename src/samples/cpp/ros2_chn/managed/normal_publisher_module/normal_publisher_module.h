// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>

#include "cpp/module_base.h"

namespace ibmw::examples::cpp::ros2_chn::normal_publisher_module {

class NormalPublisherModule : public ibmw::Module {
 public:
  NormalPublisherModule() = default;
  ~NormalPublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "NormalPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  void MainLoop();

 private:
  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;

  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_sig_;

  std::string topic_name_ = "test_topic";
  double transport_frq_;
  ibmw::transport::SenderRef publisher_;
};

}  // namespace ibmw::examples::cpp::ros2_chn::normal_publisher_module
