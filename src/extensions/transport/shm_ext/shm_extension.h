// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/engine.h"
#include "engine/extension/extension_base.h"
#include "shm_ext/shm_bus.h"

namespace ibmw::extensions::shm_extension {

// Loads the self-developed zero-copy transport (no RouDi daemon). With
// driver_name "iceoryx" it replaces the iceoryx extension without touching
// topic configuration.
class ShmExtension : public ExtensionBase {
 public:
  struct Options {
    uint64_t shm_init_size = 1024;
    std::string runtime_id;
    std::string domain;
    std::string root_dir;
    uint64_t max_bytes_per_publisher = 512ull << 20;
    bool prefault = false;
    std::string driver_name = "shm";
    // Numeric gid sharing the service directory; unset: $IBMW_SHM_GROUP.
    std::optional<gid_t> group;
    bool require_read_only = false;
  };

  ShmExtension();
  ~ShmExtension() override;

  std::string_view Name() const noexcept override { return "shm_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterShmTransportDriver();

  Options options_;
  // Heap-held: leaked if it cannot be quiesced at destruction (see dtor).
  std::unique_ptr<ShmBus> shm_bus_ = std::make_unique<ShmBus>();
};

// The extension's YAML options; throws on an invalid value.
IBMW_SHM_EXPORT ShmExtension::Options ParseShmExtensionOptions(const YAML::Node& node);
IBMW_SHM_EXPORT ShmBusOptions MakeShmBusOptions(const ShmExtension::Options& options);

// The engine picks the first driver whose name matches, so a second driver
// with the same name is silently unused. Returns who else registers
// driver_name: the built-in local transport, or a configured extension named
// "<driver_name>_extension" (e.g. iceoryx_extension for "iceoryx"; net_extension
// for tcp, udp and http); empty if nobody does.
IBMW_SHM_EXPORT std::string FindDriverNameConflict(std::string_view driver_name,
                                                   const std::vector<std::string>& extension_names);

}  // namespace ibmw::extensions::shm_extension
