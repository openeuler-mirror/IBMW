// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "extension_api/ibmw_extension_base.h"
#include "extension_api/ibmw_extension_export.h"

/// Descriptor returned by the single extension entry point.
struct ibmw_ext_entry_t {
  ibmw::EngineExtensionBase* (*create)();
  void (*destroy)(const ibmw::EngineExtensionBase*);
};

inline constexpr uint32_t kIbmwExtensionAbiVersionV2 = 2U;

// Frozen V2 loader boundary. Do not reorder, remove, or change these fields.
// This version only closes extension loading by version; a future ABI should
// replace C++ object exchange with a C registration table.
struct ibmw_ext_entry_v2_t {
  uint32_t abi_version;
  uint32_t struct_size;
  ibmw::EngineExtensionBase* (*create)();
  void (*destroy)(const ibmw::EngineExtensionBase*);
};

static_assert(std::is_standard_layout_v<ibmw_ext_entry_v2_t>);
static_assert(std::is_trivially_copyable_v<ibmw_ext_entry_v2_t>);
static_assert(offsetof(ibmw_ext_entry_v2_t, abi_version) == 0U);
static_assert(offsetof(ibmw_ext_entry_v2_t, struct_size) == sizeof(uint32_t));
static_assert(offsetof(ibmw_ext_entry_v2_t, create) == 2U * sizeof(uint32_t));
static_assert(offsetof(ibmw_ext_entry_v2_t, destroy) ==
              2U * sizeof(uint32_t) + sizeof(void*));
static_assert(sizeof(ibmw_ext_entry_v2_t) ==
              2U * sizeof(uint32_t) + 2U * sizeof(void*));

extern "C" {
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_t IbmwExtEntry();
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_v2_t IbmwExtEntryV2();
}
