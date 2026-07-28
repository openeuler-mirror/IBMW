// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "record_playback_ext/runtime/runtime_registration.h"

namespace ibmw::extensions::record_playback_extension {

class RuntimeRegistry {
 public:
  explicit RuntimeRegistry(
      std::filesystem::path registry_path = "/tmp/ibmw/runtimes")
      : registry_path_(std::move(registry_path)) {}

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

}  // namespace ibmw::extensions::record_playback_extension
