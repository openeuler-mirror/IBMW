// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

#include "base/ibmw_config.h"

// This needs to be updated when the code generator is updated
// and backward compatibility is broken.
#define IBMW_MIN_GENCODE_VERSION "0.1.0"

constexpr uint32_t VersionToInt(const char* version) {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;
  const char* p = version;

  while (*p >= '0' && *p <= '9') {
    major = major * 10 + (*p - '0');
    ++p;
  }
  if (*p != '.') throw "Invalid version format";
  ++p;

  while (*p >= '0' && *p <= '9') {
    minor = minor * 10 + (*p - '0');
    ++p;
  }
  if (*p != '.') throw "Invalid version format";
  ++p;

  while (*p >= '0' && *p <= '9') {
    patch = patch * 10 + (*p - '0');
    ++p;
  }

  return (major * 1000000) + (minor * 1000) + patch;
}

constexpr uint32_t IBMW_RUNTIME_VERSION_INT = VersionToInt(IBMW_RUNTIME_VERSION);
constexpr uint32_t IBMW_MIN_GENCODE_VERSION_INT = VersionToInt(IBMW_MIN_GENCODE_VERSION);
