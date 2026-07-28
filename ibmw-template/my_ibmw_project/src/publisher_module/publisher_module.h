// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "type_support/dds/dds_transport.h"
#include "example_event_dds_types.h"

namespace my_project {

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
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  // SenderAccess<T> bundles publisher + type info into one object.
  std::optional<ibmw::transport::SenderAccess<ibmw::protocols::example::ExampleEventMsg>> access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{false};
};

}  // namespace my_project
