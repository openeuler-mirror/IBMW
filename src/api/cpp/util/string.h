// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <string>

#include "c/util/string.h"

namespace ibmw::util {

inline ibmw_string_view_t ToIbmwStringView(const std::string& s) {
  return ibmw_string_view_t{s.c_str(), s.size()};
}

inline ibmw_string_view_t ToIbmwStringView(std::string_view s) {
  return ibmw_string_view_t{s.data(), s.size()};
}

inline ibmw_string_view_t ToIbmwStringView(const char* s) {
  return ibmw_string_view_t{s, strlen(s)};
}

inline std::string_view ToStdStringView(ibmw_string_view_t s) {
  return std::string_view(s.str, s.len);
}

inline std::string ToStdString(ibmw_string_view_t s) {
  return std::string(s.str, s.len);
}

}  // namespace ibmw::util
