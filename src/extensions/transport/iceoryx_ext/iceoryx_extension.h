// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>

#include "engine/extension/extension_base.h"
#include "engine/engine.h"
#include "iceoryx_ext/iceoryx_bus.h"

namespace ibmw::extensions::iceoryx_extension {

class IceoryxExtension : public ExtensionBase {
 public:
  struct Options {
    uint64_t shm_init_size = 1024;
    std::string runtime_id;
  };

 public:
  IceoryxExtension();
  ~IceoryxExtension() override = default;

  std::string_view Name() const noexcept override { return "iceoryx_extension"; }

  bool OnInit(runtime::core::Engine *core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterIceoryxTransportDriver();

 private:
  Options options_;

  std::atomic_bool stop_flag_ = false;

  IceoryxBus iceoryx_bus_;
};
}  // namespace ibmw::extensions::iceoryx_extension
