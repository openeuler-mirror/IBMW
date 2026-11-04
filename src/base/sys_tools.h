// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include <limits.h>
#include <unistd.h>

namespace ibmw::common::util {

/**
 * @brief get current executable path
 */
inline std::string GetExecutablePath() {
  std::string path;
  char buffer[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  // readlink truncates silently: a full buffer may be a cut-off path.
  if (len != -1 && static_cast<size_t>(len) < sizeof(buffer) - 1) {
    path.assign(buffer, static_cast<size_t>(len));
  }
  return path;
}

/**
 * @brief get current process pid
 */
inline std::string GetCurrentProcessPid() {
  return std::to_string(getpid());
}

}  // namespace ibmw::common::util
