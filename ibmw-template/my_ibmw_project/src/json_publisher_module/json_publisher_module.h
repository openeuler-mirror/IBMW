// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <optional>
#include <thread>

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "json_publisher_module_typesupport.inc.h"

namespace my_project {

class JsonPublisherModule : public ibmw::Module {
 public:
  JsonPublisherModule() = default;
  ~JsonPublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "JsonPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;
  // IBMW_TS_SENDER_ACCESS_MEMBER is defined by the generated inc.h to
  // the transport-specific SenderAccess<T> specialization. User code
  // only ever refers to `access_` through the plugin-provided publish
  // statement.
  IBMW_TS_SENDER_ACCESS_MEMBER access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{false};
  std::thread publish_thread_;
};

}  // namespace my_project
