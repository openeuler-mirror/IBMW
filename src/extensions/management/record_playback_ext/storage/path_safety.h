// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

// Resolves a metadata file path inside bag_root. An unsafe path throws; a
// missing file returns nullopt. The path is resolved once, so there is no
// window between an existence check and the containment check (EXT-044).
inline std::optional<std::filesystem::path> FindBagFilePath(
    const std::filesystem::path& bag_root,
    const std::filesystem::path& relative_path) {
  MW_ASSERT(!relative_path.empty() && relative_path.is_relative(),
            "Bag metadata file path must be relative.");
  for (const auto& component : relative_path) {
    MW_ASSERT(component != "..", "Bag metadata file path escapes bag root.");
  }
  const auto root = std::filesystem::weakly_canonical(bag_root);
  std::error_code error;
  const auto candidate = std::filesystem::canonical(root / relative_path, error);
  // Same cases std::filesystem::exists() reports as absent.
  if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
    return std::nullopt;
  }
  MW_ASSERT(!error, "Cannot resolve bag metadata file '{}': {}.",
            relative_path.string(), error.message());
  const auto rel = std::filesystem::relative(candidate, root);
  MW_ASSERT(!rel.empty() && rel != "." && rel.is_relative(),
            "Bag metadata file path escapes bag root.");
  for (const auto& component : rel) {
    MW_ASSERT(component != "..", "Bag metadata file path escapes bag root.");
  }
  return candidate;
}

inline std::filesystem::path SafeBagFilePath(
    const std::filesystem::path& bag_root,
    const std::filesystem::path& relative_path) {
  auto path = FindBagFilePath(bag_root, relative_path);
  MW_ASSERT(path.has_value(), "Bag metadata file '{}' does not exist.",
            relative_path.string());
  return std::move(*path);
}

}  // namespace ibmw::extensions::record_playback_extension
