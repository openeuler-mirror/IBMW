// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <algorithm>
#include <cstdint>

namespace ibmw::samples::transport_benchmark::iox_loaned_ctx {

template <typename Clock>
typename Clock::time_point AdvancePacingDeadline(
    typename Clock::time_point scheduled, typename Clock::duration period,
    typename Clock::time_point now) {
  if (period <= Clock::duration::zero()) return now;
  if (scheduled > now) return scheduled;
  const auto missed_periods = (now - scheduled) / period + 1;
  return scheduled + period * missed_periods;
}

inline std::chrono::nanoseconds BatchPacingPeriod(uint32_t hz,
                                                   uint32_t batch_size) {
  if (hz == 0) return std::chrono::nanoseconds::zero();
  const uint64_t numerator =
      1'000'000'000ULL * std::max<uint32_t>(batch_size, 1);
  return std::chrono::nanoseconds(std::max<uint64_t>(1, numerator / hz));
}

}  // namespace ibmw::samples::transport_benchmark::iox_loaned_ctx
