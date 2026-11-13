// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "extension_api/ibmw_extension_base.h"
#include "extension_api/ibmw_extension_export.h"

inline constexpr uint32_t kIbmwExtensionAbiVersionV3 = 3U;

// Extension ABI 3 loader boundary. Do not reorder, remove, or change these
// fields. The loader accepts only IbmwExtEntryV3; a library that exports the
// older IbmwExtEntry or IbmwExtEntryV2 was built against an earlier IBMW and is
// rejected with a request to rebuild, before any of its code is called.
struct ibmw_ext_entry_v3_t {
  uint32_t abi_version;
  uint32_t struct_size;
  ibmw::EngineExtensionBase* (*create)();
  void (*destroy)(const ibmw::EngineExtensionBase*);
};

static_assert(std::is_standard_layout_v<ibmw_ext_entry_v3_t>);
static_assert(std::is_trivially_copyable_v<ibmw_ext_entry_v3_t>);
static_assert(offsetof(ibmw_ext_entry_v3_t, abi_version) == 0U);
static_assert(offsetof(ibmw_ext_entry_v3_t, struct_size) == sizeof(uint32_t));
static_assert(offsetof(ibmw_ext_entry_v3_t, create) == 2U * sizeof(uint32_t));
static_assert(offsetof(ibmw_ext_entry_v3_t, destroy) ==
              2U * sizeof(uint32_t) + sizeof(void*));
static_assert(sizeof(ibmw_ext_entry_v3_t) ==
              2U * sizeof(uint32_t) + 2U * sizeof(void*));

extern "C" {
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_v3_t IbmwExtEntryV3();
}
