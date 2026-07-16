// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#ifndef IBMW_API_C_DRIVER_SLOT_H_
#define IBMW_API_C_DRIVER_SLOT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IBMW driver slot id (typed-slot pkg ABI).
 *
 * A driver slot id is a stable 64-bit hash derived from a transport driver's
 * stable string id (e.g. "ibmw.transport.dds"). The pkg ABI dispatches
 * driver pointers to user modules via this slot id, never via a
 * transport-name C-string (see skill R7 G6 decision in
 * ibmw/skills/transport_decoupling.md).
 *
 * Properties:
 *   - Compile-time computable from a string literal via constexpr.
 *   - Stable across .so boundaries: identical strings always hash to identical
 *     ids regardless of compiler or link order.
 *   - User pkgs can reference brand-new transports without recompilation,
 *     provided the new transport publishes a stable string id.
 *
 * Stable string id format convention:
 *   "ibmw.transport.<lowercase_name>"
 *
 * Examples:
 *   "ibmw.transport.dds", "ibmw.transport.iceoryx", "ibmw.transport.ros2",
 *   "ibmw.transport.zenoh", "ibmw.transport.mqtt",  "ibmw.transport.net".
 */
typedef uint64_t ibmw_driver_slot_t;

/**
 * @brief Reserved invalid slot id (= 0). All real ids derived from non-empty
 * strings via FNV-1a are highly unlikely to collide with this sentinel.
 */
#define IBMW_DRIVER_SLOT_INVALID ((ibmw_driver_slot_t)0)

#ifdef __cplusplus
}  // extern "C"

namespace ibmw {

/**
 * @brief FNV-1a 64-bit constexpr hash; cross-.so stable.
 *
 * Reference: http://www.isthe.com/chongo/tech/comp/fnv/
 *
 * offset_basis = 0xcbf29ce484222325
 * FNV_prime    = 0x100000001b3
 */
constexpr ibmw_driver_slot_t DriverSlotFromString(const char* s) noexcept {
  ibmw_driver_slot_t h = 0xcbf29ce484222325ULL;
  while (*s) {
    h ^= static_cast<ibmw_driver_slot_t>(static_cast<unsigned char>(*s++));
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace ibmw

/**
 * @brief Compute the slot id of a TransportDriver subclass at compile time.
 *
 * Requires DriverT to declare `static constexpr const char* kStableDriverId`.
 * Idiomatic declaration via IBMW_DECLARE_DRIVER_SLOT(...) macro in the
 * driver header (see ibmw/src/core/engine/transport/transport_driver.h).
 *
 * Usage in user pkgs:
 *   if (slot == IBMW_DRIVER_ID(MyTransportDriver)) { ... }
 */
#define IBMW_DRIVER_ID(DriverT) \
  (::ibmw::DriverSlotFromString(DriverT::kStableDriverId))

#endif  // __cplusplus

#endif  // IBMW_API_C_DRIVER_SLOT_H_
