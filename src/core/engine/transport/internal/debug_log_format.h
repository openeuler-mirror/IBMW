// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Framework-internal: serialization format used by the built-in
// `debug_log` filter when calling TrySerializeMsgWithCache /
// TrySerializeReqWithCache / TrySerializeRspWithCache.
//
// Rationale (see docs/contributing/architecture-rules.md)
//   The `debug_log` filter previously inlined the literal "json" at six
//   call sites across transport_bus.cc and service_dispatcher_yaml_and_
//   filters.cc. This created a hidden coupling between framework code and
//   the json type-support extension name (a transport-decoupling violation
//   per skills/transport_decoupling.md R7: framework should not name
//   specific extensions).
//
// Current status
//   The constant value is still "json" — semantics unchanged. This header
//   only provides a single named anchor so that:
//     1. grep'ing for transport-coupled framework code is reliable;
//     2. a configurable or negotiated debug serialization format can replace
//        it without touching the call sites again.
//
// DO NOT promote this header to public API. It is consumed only by
// framework-internal filter wiring.

#pragma once

namespace ibmw::runtime::core::transport::internal {

// Serialization format name passed to TrySerialize*WithCache from the
// `debug_log` filter. The value is the type-support name registered by the
// json extension. It remains framework-known until debug serialization becomes
// configurable or negotiated.
inline constexpr const char* kDebugLogSerializationFormat = "json";

}  // namespace ibmw::runtime::core::transport::internal
