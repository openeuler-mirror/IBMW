// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// JSON Pub/Sub demo — Publisher module.
// Publishes SensorReading using JSON type support (no IDL required).

#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "internal/typesupport/json/json_transport.h"
#include "json_custom_types.h"

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
  // SenderAccess<T> bundles publisher + type info into one object.
  std::optional<ibmw::transport::SenderAccess<SensorReading>> access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{false};
};

}  // namespace my_project
