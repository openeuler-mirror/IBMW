// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <limits>

namespace ibmw::extensions::iceoryx_extension::internal {

inline bool CheckedAddSize(size_t lhs, size_t rhs, size_t& result) noexcept {
  if (rhs > std::numeric_limits<size_t>::max() - lhs) return false;
  result = lhs + rhs;
  return true;
}

}  // namespace ibmw::extensions::iceoryx_extension::internal
