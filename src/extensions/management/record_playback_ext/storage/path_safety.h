// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

inline std::filesystem::path SafeBagFilePath(
    const std::filesystem::path& bag_root,
    const std::filesystem::path& relative_path) {
  MW_ASSERT(!relative_path.empty() && relative_path.is_relative(),
            "Bag metadata file path must be relative.");
  for (const auto& component : relative_path) {
    MW_ASSERT(component != "..", "Bag metadata file path escapes bag root.");
  }
  const auto root = std::filesystem::weakly_canonical(bag_root);
  const auto unresolved = root / relative_path;
  MW_ASSERT(std::filesystem::exists(unresolved),
            "Bag metadata file '{}' does not exist.", relative_path.string());
  const auto candidate = std::filesystem::canonical(unresolved);
  const auto rel = std::filesystem::relative(candidate, root);
  MW_ASSERT(!rel.empty() && rel != "." && rel.is_relative(),
            "Bag metadata file path escapes bag root.");
  for (const auto& component : rel) {
    MW_ASSERT(component != "..", "Bag metadata file path escapes bag root.");
  }
  return candidate;
}

}  // namespace ibmw::extensions::record_playback_extension
