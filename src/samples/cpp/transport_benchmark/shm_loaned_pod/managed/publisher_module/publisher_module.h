// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loaned_pod publisher module: drives the zero-copy POD path of the
// self-developed shm extension. On every tick it borrows a shm chunk via
// ShmRawPodPublisher, fills it with BenchPodHeader + filler bytes, and
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
#include "shm_ext/shm_pod_publisher.h"
#include "shm_ext/shm_transport_driver.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::shm_loaned_pod {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "ShmLoanedPodPublisherModule"};
  }

  // Called by main() after engine.Initialize() so the shm driver is
  // alive and discoverable through GetTransportBus().
  void SetShmDriver(
      ibmw::extensions::shm_extension::ShmTransportDriver* d) {
    driver_ = d;
  }

  // Creates the POD publisher. The shm driver only accepts POD topics in
  // its Init state, so main() calls this after Initialize() and before
  // Start(); Run() fails if it did not succeed.
  bool RegisterWithDriver();

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  ibmw::Runtime core_;
  ibmw::extensions::shm_extension::ShmTransportDriver* driver_{nullptr};
  std::unique_ptr<ibmw::extensions::shm_extension::ShmRawPodPublisher>
      raw_pub_;
  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace ibmw::samples::transport_benchmark::shm_loaned_pod
