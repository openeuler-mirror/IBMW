// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace ibmw::extensions::grpc_extension::http2 {

using Headers = std::unordered_map<std::string, std::string>;
using Trailers = std::unordered_map<std::string, std::string>;

// Headers whose values must not be logged. HTTP/2 header names are lowercase
// on the wire (RFC 9113 8.2.1).
inline bool IsSensitiveHeader(std::string_view name) noexcept {
  return name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "x-api-key";
}

// Value to log for a header: the value itself, or a placeholder if sensitive.
inline std::string_view LoggableHeaderValue(std::string_view name, std::string_view value) noexcept {
  return IsSensitiveHeader(name) ? std::string_view("<redacted>") : value;
}

}  // namespace ibmw::extensions::grpc_extension::http2
