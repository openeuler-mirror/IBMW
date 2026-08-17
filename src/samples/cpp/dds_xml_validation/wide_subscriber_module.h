// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include "SerializePoseGraph.ibmw_rpc.srv.h"
#include "cpp/module_base.h"
#include "wide_fixtures.h"

namespace ibmw::samples::dds_xml_wide {

class SubscriberModule final : public ibmw::Module {
 public:
  explicit SubscriberModule(std::function<void()> shutdown = {})
      : shutdown_(std::move(shutdown)) {}

  ibmw::ModuleDescriptor Info() const override {
    return {.module_name = "IbmwXmlWideSubscriberModule"};
  }

  bool Setup(ibmw::Runtime) override;
  bool Run() override;
  void Stop() override;

  bool Passed() const;
  bool TopicsComplete() const;
  uint32_t ReceivedTopicCount() const;
  void OnServiceRequest(bool valid);

 private:
  void RequestShutdown();

  ibmw::Runtime core_;
  std::function<void()> shutdown_;
  std::atomic<uint16_t> mask_{0};
  std::atomic<bool> service_ok_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_requested_{false};
  std::shared_ptr<slam_toolbox::srv::SerializePoseGraphSyncService> service_;
  std::thread timeout_thread_;
};

}  // namespace ibmw::samples::dds_xml_wide
