// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <cstdint>

#if IBMW_HAS_STD_SOURCE_LOCATION
  #include <source_location>
#endif

namespace ibmw::common::util {

#if IBMW_HAS_STD_SOURCE_LOCATION

using SourceLocation = std::source_location;

#else

class SourceLocation {
 public:
  constexpr SourceLocation() noexcept = default;

#if defined(__GNUC__) || defined(__clang__)
  static constexpr SourceLocation current(
      const char* file_name = __builtin_FILE(),
      std::uint_least32_t line = __builtin_LINE(),
      const char* function_name = __builtin_FUNCTION()) noexcept {
    return SourceLocation(file_name, line, function_name);
  }
#else
  static constexpr SourceLocation current(const char* file_name = "unknown",
                                          std::uint_least32_t line = 0,
                                          const char* function_name = "unknown") noexcept {
    return SourceLocation(file_name, line, function_name);
  }
#endif

  [[nodiscard]] constexpr const char* file_name() const noexcept { return file_name_; }
  [[nodiscard]] constexpr std::uint_least32_t line() const noexcept { return line_; }
  [[nodiscard]] constexpr const char* function_name() const noexcept { return function_name_; }

 private:
  constexpr SourceLocation(const char* file_name,
                           std::uint_least32_t line,
                           const char* function_name) noexcept
      : file_name_(file_name),
        line_(line),
        function_name_(function_name) {}

  const char* file_name_ = "unknown";
  std::uint_least32_t line_ = 0;
  const char* function_name_ = "unknown";
};

#endif

}  // namespace ibmw::common::util
