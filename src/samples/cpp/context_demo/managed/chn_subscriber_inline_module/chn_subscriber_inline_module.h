// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#include "cpp/ibmw_sdk.h"
#include "example_event_dds_types.h"

namespace ibmw::examples::cpp::context::ChnSubscriberInlineModule {
class ChnSubscriberInlineModule : public ibmw::Module {
 public:
  ChnSubscriberInlineModule() = default;
  ~ChnSubscriberInlineModule() override = default;

  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "ContextSubscriberInlineModule"}; }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
  ibmw::context::res::Subscriber<ibmw::protocols::example::ExampleEventMsg> subscriber_inline_;

  std::string topic_name_;
};

}  // namespace ibmw::examples::cpp::context::ChnSubscriberInlineModule
