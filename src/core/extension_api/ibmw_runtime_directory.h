// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace ibmw::extension {

inline std::filesystem::path DefaultRuntimeRegistryPath() {
  if (const char* directory = std::getenv("XDG_RUNTIME_DIR"); directory && *directory) {
    return std::filesystem::path(directory) / "ibmw";
  }
  return std::filesystem::path("/tmp") / ("ibmw-" + std::to_string(::geteuid()));
}

}  // namespace ibmw::extension
