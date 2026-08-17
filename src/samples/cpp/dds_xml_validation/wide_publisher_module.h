// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

#include "SerializePoseGraph.ibmw_rpc.srv.h"
#include "cpp/module_base.h"
#include "wide_fixtures.h"

namespace ibmw::samples::dds_xml_wide {

class PublisherModule final : public ibmw::Module {
 public:
  explicit PublisherModule(std::function<void()> shutdown = {})
      : shutdown_(std::move(shutdown)) {}

  ibmw::ModuleDescriptor Info() const override {
    return {.module_name = "IbmwXmlWidePublisherModule"};
  }

  bool Setup(ibmw::Runtime) override;
  bool Run() override;
  void Stop() override;

  bool Passed() const;
  uint32_t SentTopicCount() const;

 private:
  void Loop();
  void RequestShutdown();

  ibmw::Runtime core_;
  std::function<void()> shutdown_;
  std::atomic<bool> running_{false};
  std::atomic<bool> registrations_ok_{false};
  std::atomic<bool> matrix_sent_{false};
  std::atomic<bool> service_ok_{false};
  std::atomic<bool> shutdown_requested_{false};
  std::thread thread_;
  ibmw::transport::SenderRef image_;
  ibmw::transport::SenderRef compressed_;
  ibmw::transport::SenderRef imu_;
  ibmw::transport::SenderRef scan_;
  ibmw::transport::SenderRef cloud_;
  ibmw::transport::SenderRef camera_;
  ibmw::transport::SenderRef odom_;
  ibmw::transport::SenderRef path_;
  ibmw::transport::SenderRef grid_;
  ibmw::transport::SenderRef metadata_;
  ibmw::transport::SenderRef tf_;
  ibmw::transport::SenderRef marker_;
  ibmw::transport::SenderRef diagnostic_;
};

}  // namespace ibmw::samples::dds_xml_wide
