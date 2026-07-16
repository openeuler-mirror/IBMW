// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

namespace ibmw::examples::dds_image_demo {

class ImageSubscriberModule : public ibmw::Module {
 public:
  ImageSubscriberModule() = default;
  ~ImageSubscriberModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ImageSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::Runtime core_;
  ibmw::transport::TransportRef transport_;
};

}  // namespace ibmw::examples::dds_image_demo
