// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// IBMW pkg ABI — typed-slot driver dispatch.
//
// A pkg .so exports a single C entry point, IbmwPkgEntryV2(), returning an
// ibmw_pkg_entry_v2_t descriptor with callbacks to enumerate, create, and
// destroy modules, plus an optional typed-slot driver injection callback.
//
// The driver injection path uses a stable 64-bit slot id (FNV-1a hash of
// the transport stable id; see api/c/driver_slot.h) instead of a
// transport-name C-string. The host loader broadcasts (slot_id,
// driver_ptr) pairs after Engine::Initialize and before Engine::Start;
// modules opt in to the transports they want via the IBMW_USE_DRIVER /
// IBMW_USE_DRIVERS macros (see cpp/module_base.h). User pkgs do not need
// to know the host's transport name set at compile time.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "c/driver_slot.h"
#include "c/module_base.h"
#include "pkg/pkg_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Package entry ABI version. The loader rejects any other value, and a
/// library exporting only the older unversioned IbmwPkgEntry() is rejected
/// with a request to rebuild it.
#define IBMW_PKG_ABI_VERSION_V2 2

/// Descriptor returned by IbmwPkgEntryV2().
///
/// `abi_version` must be IBMW_PKG_ABI_VERSION_V2 and `struct_size` at least
/// sizeof(ibmw_pkg_entry_v2_t); both are checked before any callback is used.
///
/// The first four callbacks (get_module_num, get_module_name_list,
/// create_module, destroy_module) are MANDATORY and must be non-null.
///
/// `set_driver_slot` is OPTIONAL (may be null) for pkgs that have no
/// modules requesting any driver injection. Host loader broadcasts every
/// used (slot_id, driver_ptr) pair to every loaded pkg after
/// Engine::Initialize and before Engine::Start.
typedef struct {
  uint32_t abi_version;
  uint32_t struct_size;

  size_t (*get_module_num)();
  const ibmw_string_view_t* (*get_module_name_list)();
  const ibmw_module_base_t* (*create_module)(ibmw_string_view_t module_name);
  void (*destroy_module)(const ibmw_module_base_t* module_ptr);

  /// OPTIONAL. May be null in pkgs that need no driver injection.
  /// Host invokes once per (loaded_module, used_transport_driver) pair
  /// after Engine::Initialize, before Engine::Start. The pkg dispatches
  /// to the matching module's ibmw::Module::SetDriverSlot virtual.
  void (*set_driver_slot)(ibmw_string_view_t module_name,
                          ibmw_driver_slot_t slot_id,
                          void* driver_ptr);
} ibmw_pkg_entry_v2_t;

/**
 * @brief Single entry point for an IBMW module package.
 *
 * Returns a descriptor with callbacks to enumerate, create, and destroy
 * modules provided by this shared library, plus a typed-slot driver
 * injection callback.
 */
IBMW_PKG_EXPORT ibmw_pkg_entry_v2_t IbmwPkgEntryV2();

#ifdef __cplusplus
}
#endif
