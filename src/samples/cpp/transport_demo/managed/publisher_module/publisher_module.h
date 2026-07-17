// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"
#include "internal/typesupport/dds/dds_transport.h"

namespace pubsub_demo {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

 private:
  ibmw::Runtime core_;
  ibmw::transport::SenderRef publisher_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{false};
};

}  // namespace pubsub_demo
