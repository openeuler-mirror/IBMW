// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "generated/record_demo_publishers.h"

namespace ibmw::examples::cpp::record_demo::cdr_publisher_module {

class CdrPublisherModule final : public ibmw::Module {
 public:
  CdrPublisherModule() = default;
  ~CdrPublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "CdrPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;
  generated::CdrPublisher publisher_;
  std::string topic_name_ = "record_demo_cdr_topic";
  double transport_frq_ = 1.0;
  std::atomic_bool running_{false};
  std::thread worker_;
  uint64_t sequence_ = 0;
};

}  // namespace ibmw::examples::cpp::record_demo::cdr_publisher_module
