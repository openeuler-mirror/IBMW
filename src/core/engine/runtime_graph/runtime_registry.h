// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "engine/runtime_graph/runtime_registration.h"

namespace ibmw::runtime::core::runtime_graph {

class RuntimeRegistry {
 public:
  explicit RuntimeRegistry(
      std::filesystem::path registry_path = ibmw::extension::DefaultRuntimeRegistryPath())
      : registry_path_(ibmw::extension::RuntimeRegistryDirectory(registry_path)) {}

  std::vector<RuntimeRegistration> DiscoverLocalRuntimes() const;
  std::vector<TopicAdvertisement> FindTopic(
      std::string_view topic_name) const;

  const std::filesystem::path& RegistryPath() const noexcept {
    return registry_path_;
  }

 private:
  static bool IsProcessAlive(int64_t pid) noexcept;

  std::filesystem::path registry_path_;
};

}  // namespace ibmw::runtime::core::runtime_graph
