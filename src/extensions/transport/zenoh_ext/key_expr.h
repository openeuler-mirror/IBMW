// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <string_view>

namespace ibmw::extensions::zenoh_extension {

// limit_domain is kept without surrounding '/', the form discovery and the
// record/topic tools use. "", "/" and "/robot/" normalize to "" and "robot".
inline std::string NormalizeLimitDomain(std::string_view domain) {
  while (domain.starts_with('/')) domain.remove_prefix(1);
  while (domain.ends_with('/')) domain.remove_suffix(1);
  return std::string(domain);
}

// Suffix appended to transport/RPC key expressions; empty for the empty domain
// so keys never end with '/'.
inline std::string LimitDomainKeySuffix(std::string_view domain) {
  return domain.empty() ? std::string() : "/" + std::string(domain);
}

}  // namespace ibmw::extensions::zenoh_extension
