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

#define MW_ASSERT(__expr__, __fmt__, ...)                        \
  do {                                                            \
    if (!(__expr__)) [[unlikely]] {                               \
      auto __mw_loc__ = ::ibmw::common::util::SourceLocation::current(); \
      throw ibmw::common::util::IbmwException(                    \
          ::ibmw_fmt::format("[{}:{} @{}]: " __fmt__,             \
                              __mw_loc__.file_name(),             \
                              __mw_loc__.line(),                  \
                              __FUNCTION__, ##__VA_ARGS__));       \
    }                                                             \
  } while (0)

#define MW_ASSERT_LOC(__location__, __expr__, __fmt__, ...)       \
  do {                                                            \
    if (!(__expr__)) [[unlikely]] {                               \
      throw ibmw::common::util::IbmwException(                    \
          ::ibmw_fmt::format("[{}:{}]: " __fmt__,                 \
                              __location__.file_name(),           \
                              __location__.line(),                \
                              ##__VA_ARGS__));                     \
    }                                                             \
  } while (0)
