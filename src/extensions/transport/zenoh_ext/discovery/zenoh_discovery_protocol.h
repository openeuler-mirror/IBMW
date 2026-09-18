// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "extension_api/ibmw_discovery.h"

namespace ibmw::discovery::zenoh_protocol {

inline constexpr size_t kMaxSnapshotBytes = 4 * 1024 * 1024;
inline constexpr size_t kMaxEndpoints = 10000;

struct Snapshot {
  std::string instance;
  std::string domain;
  uint64_t revision = 0;
  std::vector<DiscoveredEndpoint> endpoints;
};

// Hex encodes every domain byte into one literal key segment. Empty domain
// maps to "d"; slash, wildcard and percent characters cannot change routing.
std::string DomainPrefix(std::string_view domain);
std::string SnapshotKey(std::string_view domain, std::string_view instance);
bool IsConcreteKey(std::string_view key) noexcept;
std::string Encode(const Snapshot& snapshot);
Snapshot Decode(std::string_view payload, std::string_view reply_key,
                const DiscoverySource& source);

}  // namespace ibmw::discovery::zenoh_protocol
