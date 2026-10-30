// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iox_loaned_pod publisher module: drives the zero-copy POD path of the
// iceoryx extension. On every tick it borrows an SHM chunk via
// IoxRawPodPublisher, fills it with BenchPodHeader + filler bytes, and
// publishes -- no IBMW serialization, no buffer-array allocator, no copy.
//
// The driver pointer is injected by the standalone publisher_app after
// engine.Initialize() returns (same dynamic_cast pattern as the DDS
// loaned_message_demo).

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "cpp/module_base.h"

#include "bench_options.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "iceoryx_ext/iox_style_publisher.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::iox_loaned_pod {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "IoxLoanedPodPublisherModule"};
  }

  // Called by main() after engine.Initialize() so the iceoryx driver is
  // alive and discoverable through GetTransportBus().
  void SetIceoryxDriver(
      ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* d) {
    driver_ = d;
  }

  // Creates the POD publisher. The iceoryx driver only accepts POD topics in
  // its Init state, so main() calls this after Initialize() and before
  // Start(); Run() fails if it did not succeed.
  bool RegisterWithDriver();

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* driver_{nullptr};
  std::shared_ptr<ibmw::extensions::iceoryx_extension::IoxRawPodPublisher>
      raw_pub_;
  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_pod
