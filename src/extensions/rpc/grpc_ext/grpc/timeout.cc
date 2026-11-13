// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "grpc_ext/grpc/timeout.h"

#include <cstdint>
#include <utility>

namespace ibmw::extensions::grpc_extension::grpc {

// "grpc-timeout" is 1 to 8 ASCII digits and a unit. Malformed values are
// rejected instead of throwing; values beyond the nanosecond range (from
// about 2562048 hours on) saturate instead of overflowing.
std::optional<std::chrono::nanoseconds> ParseTimeout(const std::string& timeout_str) {
  constexpr size_t kMaxDigits = 8;
  const size_t n = timeout_str.size();
  if (n < 2 || n > kMaxDigits + 1) return std::nullopt;
  int64_t value = 0;
  for (size_t i = 0; i + 1 < n; ++i) {
    const char c = timeout_str[i];
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
  }

  int64_t unit_ns = 0;
  switch (timeout_str[n - 1]) {
    case 'H':
      unit_ns = 3600000000000;
      break;
    case 'M':
      unit_ns = 60000000000;
      break;
    case 'S':
      unit_ns = 1000000000;
      break;
    case 'm':
      unit_ns = 1000000;
      break;
    case 'u':
      unit_ns = 1000;
      break;
    case 'n':
      unit_ns = 1;
      break;
    default:
      return std::nullopt;
  }
  if (value > std::chrono::nanoseconds::max().count() / unit_ns) return std::chrono::nanoseconds::max();
  return std::chrono::nanoseconds(value * unit_ns);
}

// Picks the finest unit that fits in 8 digits, rounding up so that the peer
// never sees a shorter deadline; longer timeouts saturate at 99999999H.
std::string FormatTimeout(std::chrono::nanoseconds timeout) {
  constexpr int64_t kMaxValue = 99999999;
  static constexpr std::pair<int64_t, char> kUnits[] = {
      {1, 'n'}, {1000, 'u'}, {1000000, 'm'}, {1000000000, 'S'}, {60000000000, 'M'}, {3600000000000, 'H'}};
  const int64_t ns = timeout.count() < 0 ? 0 : timeout.count();
  for (const auto& [unit_ns, unit] : kUnits) {
    const int64_t value = ns / unit_ns + (ns % unit_ns != 0 ? 1 : 0);
    if (value <= kMaxValue) return std::to_string(value) + unit;
  }
  return std::to_string(kMaxValue) + 'H';
}

}  // namespace ibmw::extensions::grpc_extension::grpc
