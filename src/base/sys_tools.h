// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <limits.h>
  #include <unistd.h>
#endif

namespace ibmw::common::util {

/**
 * @brief get current executable path
 */
inline std::string GetExecutablePath() {
  std::string path;
#ifdef _WIN32
  // A path longer than the buffer is truncated: return empty instead.
  char buffer[MAX_PATH];
  const DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
  if (len != 0 && len < MAX_PATH) {
    path.assign(buffer, len);
  }
#else
  char buffer[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  // readlink truncates silently: a full buffer may be a cut-off path.
  if (len != -1 && static_cast<size_t>(len) < sizeof(buffer) - 1) {
    path.assign(buffer, static_cast<size_t>(len));
  }
#endif
  return path;
}

/**
 * @brief get current process pid
 */
inline std::string GetCurrentProcessPid() {
#ifdef _WIN32
  return std::to_string(GetCurrentProcessId());
#else
  return std::to_string(getpid());
#endif
}

}  // namespace ibmw::common::util
