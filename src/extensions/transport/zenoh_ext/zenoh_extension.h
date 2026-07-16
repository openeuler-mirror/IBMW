// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include "engine/extension/extension_base.h"
#include "engine/engine.h"
#include "zenoh_ext/zenoh_transport_driver.h"
#include "zenoh_ext/zenoh_rpc_driver.h"

namespace ibmw::extensions::zenoh_extension {
class ZenohExtension : public ExtensionBase {
 public:
  struct Options {
    std::string native_cfg_path;
    std::string limit_domain;
    size_t shm_pool_size = static_cast<size_t>(1024 * 1024 * 10);  // default: 10 MB
    size_t shm_init_loan_size = 1024;                              // default: 1 KB
  };

 public:
  ZenohExtension();
  ~ZenohExtension() override = default;

  std::string_view Name() const noexcept override { return "zenoh_extension"; }

  bool OnInit(runtime::core::Engine *core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterZenohTransportDriver();
  void RegisterZenohRpcDriver();

 private:
  Options options_;

  std::atomic_bool stop_flag_ = false;

  std::shared_ptr<ZenohBus> zenoh_bus_ptr_ = std::make_shared<ZenohBus>();
};

}  // namespace ibmw::extensions::zenoh_extension
