// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

#include "c/service/rpc_context_base.h"
#include "c/transport/transport_context_base.h"

namespace ibmw::extensions::net_extension {

// Context meta received from a peer must not override framework-owned "ibmw-"
// keys (driver, serialization type, function name, routing). The sender's
// correlation stamps (publish sequence/timestamp, rpc request id) are
// informational and pass through.
inline bool IsPeerMetaKeyAllowed(std::string_view key) noexcept {
  constexpr std::string_view kReservedPrefix = IBMW_TRANSPORT_CONTEXT_KEY_PREFIX;
  if (key.substr(0, kReservedPrefix.size()) != kReservedPrefix) return true;
  return key == IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ || key == IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP ||
         key == IBMW_RPC_CONTEXT_KEY_REQUEST_ID;
}

}  // namespace ibmw::extensions::net_extension
