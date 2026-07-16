// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <future>
#include <memory>
#include <string>

#include "cpp/ibmw_sdk.h"
#include "example_event_dds_types.h"

namespace ibmw::examples::cpp::context::transport_publisher_module {

class ChnPublisherModule : public ibmw::Module {
 public:
  ChnPublisherModule() = default;
  ~ChnPublisherModule() = default;
  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ContextTransportPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void RunPublishLoopTask();

 private:
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
  ibmw::executor::ExecutorHandle work_executor_;
  ibmw::context::res::Publisher<ibmw::protocols::example::ExampleEventMsg> publisher_;

  std::string topic_name_;
  std::string executor_name_ = "work_executor";
  double transport_frq_ = 1.0;

  std::shared_ptr<std::promise<void>> publish_loop_exit_signal_;
  std::future<void> publish_loop_exit_future_;
};

}  // namespace ibmw::examples::cpp::context::transport_publisher_module
