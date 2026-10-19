// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <optional>
#include <set>

#include "engine/discovery/discovery_provider.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "record_playback_ext/coordinator/bag_capture_planner.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

struct RecordDiscoveryOptions {
  ibmw::discovery::DiscoveryOptions source;
  std::filesystem::path config_file;
  std::optional<uint32_t> domain;
  std::optional<ibmw::discovery::DiscoveryBackend> selected_backend;
  std::optional<std::string> zenoh_domain;
  bool registry_selected = false;

  bool ParseArgument(int& index, int argc, const char* const* argv);
  void Validate() const;
  bool Network() const noexcept {
    return source.backend != ibmw::discovery::DiscoveryBackend::kRegistry;
  }
};

struct RecordConnection {
  std::string driver;
  YAML::Node runtime_config;
};

// Read executable packages and transport options only from recorder-owned
// configuration, never from network-discovered library paths.
RecordConnection LoadRecordConnection(
    RecordDiscoveryOptions& options,
    runtime::core::runtime_graph::TypeSupportRegistry& types);

CaptureGraph ResolveNetworkCaptureGraph(
    const ibmw::discovery::DiscoverySnapshot& snapshot,
    const runtime::core::runtime_graph::TypeSupportRegistry& types);
void ValidateCaptureSchemas(const CaptureGraph& graph,
                            const std::set<std::string>& selected_topics);

class RecordDiscovery {
 public:
  RecordDiscovery(const RecordDiscoveryOptions& options,
                  const runtime::core::runtime_graph::TypeSupportRegistry& types);
  ~RecordDiscovery();
  CaptureGraph Query(std::chrono::milliseconds timeout = {});

 private:
  ibmw::discovery::DiscoveryOptions options_;
  const runtime::core::runtime_graph::TypeSupportRegistry& types_;
  std::unique_ptr<ibmw::discovery::DiscoveryProvider> provider_;
};

}  // namespace ibmw::extensions::record_playback_extension
