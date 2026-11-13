// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "base/format.h"
#include "base/source_location.h"

namespace ibmw::common::util {

class IbmwException : public std::exception {
 public:
  template <typename... Args>
    requires std::constructible_from<std::string, Args...>
  explicit IbmwException(Args&&... args)
      : err_msg_(std::forward<Args>(args)...) {}

  ~IbmwException() noexcept override = default;

  const char* what() const noexcept override { return err_msg_.c_str(); }

 private:
  std::string err_msg_;
};

}  // namespace ibmw::common::util

// No local variables and no reserved names, so caller arguments are never
// shadowed (API-050). Only the failing branch evaluates the location.
#define MW_ASSERT(mw_expr, mw_fmt, ...)                                    \
  do {                                                                    \
    if (!(mw_expr)) [[unlikely]] {                                        \
      throw ibmw::common::util::IbmwException(::ibmw_fmt::format(         \
          "[{}:{} @{}]: " mw_fmt,                                         \
          ::ibmw::common::util::SourceLocation::current().file_name(),    \
          ::ibmw::common::util::SourceLocation::current().line(),         \
          __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__));                      \
    }                                                                     \
  } while (0)

#define MW_ASSERT_LOC(mw_location, mw_expr, mw_fmt, ...)                   \
  do {                                                                    \
    if (!(mw_expr)) [[unlikely]] {                                        \
      throw ibmw::common::util::IbmwException(                            \
          ::ibmw_fmt::format("[{}:{}]: " mw_fmt,                          \
                             (mw_location).file_name(),                   \
                             (mw_location).line()                         \
                             __VA_OPT__(, ) __VA_ARGS__));                \
    }                                                                     \
  } while (0)
