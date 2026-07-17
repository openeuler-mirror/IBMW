// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "image_subscriber_typesupport.inc.h"

namespace dds_image_demo {

class ImageSubscriber : public ibmw::Module {
 public:
  ImageSubscriber() = default;
  ~ImageSubscriber() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "ImageSubscriber"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
};

}  // namespace dds_image_demo
