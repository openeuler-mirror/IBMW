// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

#include <boost/beast/core/string.hpp>

namespace ibmw::common::net {

/**
 * @brief Media type part of a Content-Type value, without parameters
 * @note "application/json; charset=utf-8" -> "application/json". Leading
 * whitespace is skipped; the type ends at the first ';', space or tab.
 */
inline std::string_view HttpMediaType(std::string_view content_type) noexcept {
  auto begin = content_type.find_first_not_of(" \t");
  if (begin == std::string_view::npos) return {};
  content_type.remove_prefix(begin);
  return content_type.substr(0, content_type.find_first_of("; \t"));
}

/**
 * @brief Whether a Content-Type value has the given media type
 * @note Media types are case-insensitive (RFC 9110 §8.3.1); parameters are ignored.
 */
inline bool HttpMediaTypeIs(std::string_view content_type, std::string_view media_type) noexcept {
  return boost::beast::iequals(HttpMediaType(content_type), media_type);
}

}  // namespace ibmw::common::net
