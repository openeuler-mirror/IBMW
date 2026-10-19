// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once

#include <dlfcn.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ibmw::runtime::core::topic_io {
inline std::filesystem::path TransportLibrary(std::string_view extension) {
  static const char anchor = 0;
  Dl_info info{};
  if (!::dladdr(&anchor, &info) || !info.dli_fname)
    throw std::runtime_error("Cannot locate the transport library directory; specify a library path.");
  const auto directory = std::filesystem::absolute(info.dli_fname).parent_path();
  const auto name = "libibmw_" + std::string(extension) + "_transport.so";
  const auto adjacent = directory / name;
  if (std::filesystem::is_regular_file(adjacent)) return adjacent;
  // Build/staged libraries are colocated with the CLI provider. Installation
  // places the provider in bin, but linkable transports such as DDS in lib.
  const auto installed = directory.parent_path() / "lib" / name;
  if (std::filesystem::is_regular_file(installed)) return installed;
  return adjacent;  // Preserve the loader's diagnostic when neither exists.
}
}  // namespace ibmw::runtime::core::topic_io
