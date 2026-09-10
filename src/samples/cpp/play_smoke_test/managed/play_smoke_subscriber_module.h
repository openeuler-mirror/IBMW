// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "cpp/module_base.h"
#include "generated/play_smoke_subscriber.h"

namespace ibmw::examples::cpp::play_smoke_test {

class PlaySmokeSubscriberModule final : public ibmw::Module {
 public:
  PlaySmokeSubscriberModule(std::string name, std::string serialization);
  ~PlaySmokeSubscriberModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = name_};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  bool Subscribe(ibmw::transport::ReceiverRef subscriber,
                 const std::string& topic);
  void Receive(
      const std::string& topic, generated::ReceivedMessage message);

  const std::string name_;
  const std::string serialization_;
  ibmw::Runtime core_;
  std::vector<std::string> topics_;
  std::vector<ibmw::transport::ReceiverRef> subscribers_;
  std::filesystem::path output_path_;
  std::ofstream output_;
  std::mutex output_mutex_;
  std::atomic_uint64_t received_{0};
};

}  // namespace ibmw::examples::cpp::play_smoke_test
