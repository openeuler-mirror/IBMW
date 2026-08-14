// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <string>

#include "cpp/module_base.h"
#include "generated/playback_demo_subscriber.h"

namespace ibmw::examples::cpp::playback_demo::subscriber_module {

class SubscriberModule final : public ibmw::Module {
 public:
  SubscriberModule() = default;
  ~SubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PlaybackSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  ibmw::Runtime core_;
  ibmw::transport::ReceiverRef subscriber_;
  generated::PlaybackSubscriber subscriber_adapter_;
  std::string topic_name_ = "playback_demo_topic";
  std::atomic_uint64_t received_count_{0};
};

}  // namespace ibmw::examples::cpp::playback_demo::subscriber_module
