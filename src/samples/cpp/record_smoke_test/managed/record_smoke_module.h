// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "cpp/module_base.h"

namespace ibmw::examples::cpp::record_smoke_test {

class RecordSmokeModule final : public ibmw::Module {
 public:
  RecordSmokeModule(std::string module_name, std::string message_kind);
  ~RecordSmokeModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = module_name_};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  bool RegisterPublisher(ibmw::transport::SenderRef publisher);
  bool RegisterSubscriber(
      ibmw::transport::ReceiverRef subscriber,
      const std::string& topic);
  void PublishLoop();
  void Publish(ibmw::transport::SenderRef publisher, uint64_t sequence);
  void PublishClock(uint64_t timestamp_ns);

  const std::string module_name_;
  const std::string message_kind_;
  ibmw::Runtime core_;
  std::vector<std::string> publish_topics_;
  std::vector<std::string> subscribe_topics_;
  std::vector<ibmw::transport::SenderRef> publishers_;
  std::vector<ibmw::transport::ReceiverRef> subscribers_;
  ibmw::transport::SenderRef clock_publisher_;
  std::string clock_topic_;
  double frequency_hz_ = 10.0;
  uint64_t clock_start_ns_ = 100123456789ull;
  uint64_t clock_step_ns_ = 40000000ull;
  uint32_t clock_delay_ms_ = 0;
  std::atomic_bool running_{false};
  std::atomic_uint64_t received_count_{0};
  std::thread worker_;
};

}  // namespace ibmw::examples::cpp::record_smoke_test
