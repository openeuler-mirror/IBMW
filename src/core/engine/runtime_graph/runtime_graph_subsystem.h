// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "cpp/util/schema_support.h"
#include "cpp/util/type_support.h"
#include "engine/isubsystem.h"
#include "engine/runtime_graph/runtime_graph_agent.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "engine/topic_io/runtime_tap_stream.h"
#include "extension_api/ibmw_extension_runtime.h"
#include "extension_api/ibmw_runtime_graph.h"

namespace ibmw::runtime::core {
class Engine;
}

namespace ibmw::runtime::core::runtime_graph {

class RuntimeGraphSubsystem final : public ISubsystem,
                                    public ibmw::extension::RuntimeGraphV1 {
 public:
  struct Options {
    bool enabled = true;
    std::string runtime_id;
    std::filesystem::path registry_path = ibmw::extension::DefaultRuntimeRegistryPath();
    std::vector<std::string> type_support_pkgs;
  };

  RuntimeGraphSubsystem() = default;
  ~RuntimeGraphSubsystem() override = default;

  RuntimeGraphSubsystem(const RuntimeGraphSubsystem&) = delete;
  RuntimeGraphSubsystem& operator=(const RuntimeGraphSubsystem&) = delete;

  void Configure(Options options);
  void Configure(YAML::Node options_node);
  void ApplyFallbackOptions(const ibmw::extension::RuntimeGraphOptions& options) override;
  void SetEngine(ibmw::runtime::core::Engine* engine) noexcept { engine_ = engine; }

  void RegisterTypeSupportPackage(std::string package_path);
  ibmw::util::TypeSupportRef FindTypeSupport(
      std::string_view msg_type,
      std::string_view package_path = {}) const;
  std::string TypeSupportPackage(
      std::string_view msg_type,
      std::string_view package_path = {}) const;
  std::optional<ibmw::util::SchemaDescriptor> FindSchema(
      std::string_view msg_type,
      std::string_view serialization_type) const;
  void HideModulePublishers(std::string module_name);

  void SetStartTapHandler(RuntimeGraphAgent::StartTapHandler handler);
  void SetStopTapHandler(RuntimeGraphAgent::StopTapHandler handler);
  void SetStartPlaybackHandler(RuntimeGraphAgent::StartPlaybackHandler handler);
  void SetStopPlaybackHandler(RuntimeGraphAgent::StopPlaybackHandler handler);

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  // Capture app-mode contexts added after Initialize, before ModuleBus starts.
  void RefreshModuleList();
  // Capture app-mode RPC registrations before the Service subsystem starts.
  void RefreshServiceList();

  const RuntimeRegistration& Registration() const;
  bool Enabled() const noexcept { return options_.enabled; }
  const std::filesystem::path& RegistryPath() const noexcept {
    return options_.registry_path;
  }

 private:
  RuntimeRegistration BuildRegistration() const;
  void LoadConfiguredTypeSupportPackages();
  void DiscoverTypeSupportPackagesFromCatalog();

  Options options_;
  YAML::Node explicit_options_;
  ibmw::runtime::core::Engine* engine_ = nullptr;
  TypeSupportRegistry type_support_registry_;
  std::unordered_set<std::string> hidden_module_publishers_;
  RuntimeGraphAgent::StartTapHandler start_tap_handler_;
  RuntimeGraphAgent::StopTapHandler stop_tap_handler_;
  RuntimeGraphAgent::StartPlaybackHandler start_playback_handler_;
  RuntimeGraphAgent::StopPlaybackHandler stop_playback_handler_;
  std::unique_ptr<ibmw::extension::RuntimeV2> tap_runtime_;
  std::unique_ptr<topic_io::PublishTapCapability> publish_tap_;
  std::unique_ptr<topic_io::RuntimeTapServer> tap_server_;
  std::unique_ptr<RuntimeGraphAgent> agent_;
  RuntimeRegistration registration_;
  bool configured_ = false;
};

}  // namespace ibmw::runtime::core::runtime_graph
