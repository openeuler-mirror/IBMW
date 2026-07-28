// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ibmw::transport {

struct MessageIdentity {
  std::string source_id;
  std::string publisher_id;
  uint64_t sequence = 0;

  bool operator==(const MessageIdentity&) const = default;

  bool IsValid() const noexcept {
    return !source_id.empty() && !publisher_id.empty();
  }
};

struct MessageIdentityHash {
  size_t operator()(const MessageIdentity& identity) const noexcept {
    const size_t source_hash = std::hash<std::string>{}(identity.source_id);
    const size_t publisher_hash =
        std::hash<std::string>{}(identity.publisher_id);
    const size_t sequence_hash = std::hash<uint64_t>{}(identity.sequence);
    size_t result = source_hash;
    result ^= publisher_hash + 0x9e3779b9 + (result << 6) + (result >> 2);
    result ^= sequence_hash + 0x9e3779b9 + (result << 6) + (result >> 2);
    return result;
  }
};

}  // namespace ibmw::transport
