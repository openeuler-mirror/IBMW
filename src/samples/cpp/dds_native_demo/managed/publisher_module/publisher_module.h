// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"

namespace ibmw::examples::dds_native_demo {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "DdsPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

 private:
  ibmw::Runtime core_;
  ibmw::transport::TransportRef transport_;

  std::atomic<bool> running_{false};
  std::thread publish_thread_;
};

}  // namespace ibmw::examples::dds_native_demo
