// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "internal/typesupport/dds/dds_transport.h"
#include "my_msg/my_msg_dds_types.h"

namespace my_project {

class MyMsgPublisherModule : public ibmw::Module {
 public:
  MyMsgPublisherModule() = default;
  ~MyMsgPublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "MyMsgPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  std::optional<ibmw::transport::SenderAccess<my_project::my_msg::MyMsg>> access_;
  uint64_t seq_ = 0;
  std::atomic<bool> running_{false};
};

}  // namespace my_project
