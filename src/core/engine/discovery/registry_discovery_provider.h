// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <optional>

#include "engine/discovery/discovery_provider.h"
#include "engine/runtime_graph/runtime_registry.h"

namespace ibmw::discovery {

class RegistryDiscoveryProvider final : public DiscoveryProvider {
 public:
  void Start(const DiscoveryOptions& options) override;
  DiscoverySnapshot Query(
      std::chrono::steady_clock::time_point deadline) override;
  void Stop() noexcept override;

  // Registry control-plane details from the most recent Query. These stay out
  // of the network discovery ABI and expire on the next Query or Stop.
  const std::vector<ibmw::runtime::core::runtime_graph::RuntimeRegistration>&
  Runtimes() const noexcept { return runtimes_; }

 private:
  DiscoverySource source_;
  std::optional<ibmw::runtime::core::runtime_graph::RuntimeRegistry> registry_;
  std::vector<ibmw::runtime::core::runtime_graph::RuntimeRegistration> runtimes_;
};

}  // namespace ibmw::discovery
