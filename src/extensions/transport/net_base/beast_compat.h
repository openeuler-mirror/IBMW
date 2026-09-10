// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

#include <boost/beast/core/string.hpp>

namespace ibmw::common::net {

inline std::string_view ToStdStringView(boost::beast::string_view value) noexcept {
  return {value.data(), value.size()};
}

inline boost::beast::string_view ToBeastStringView(std::string_view value) noexcept {
  return {value.data(), value.size()};
}

}  // namespace ibmw::common::net
