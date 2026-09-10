// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "c/util/string.h"
#include "extension_api/ibmw_extension_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Versioned command callback exposed by a CLI provider. */
typedef int (*ibmw_cli_command_invoke_v1_t)(
    int argc, const char* const* argv);

/** One top-level command registered by a CLI provider. */
typedef struct {
  ibmw_string_view_t name;
  ibmw_string_view_t description;
  ibmw_cli_command_invoke_v1_t invoke;
} ibmw_cli_command_v1_t;

/** Descriptor returned by the generic CLI provider entry point. */
typedef struct {
  uint32_t abi_version;
  uint32_t struct_size;
  ibmw_string_view_t provider_name;
  size_t command_count;
  const ibmw_cli_command_v1_t* commands;
} ibmw_cli_entry_v1_t;

/** ABI version for ibmw_cli_entry_v1_t. */
enum { kIbmwCliAbiVersionV1 = 1U };

/**
 * Optional entry point implemented by extensions that provide CLI commands.
 *
 * The returned descriptor and command array must remain valid while the
 * provider library is loaded. The host keeps the library loaded while a
 * command is executing.
 */
IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1();

#ifdef __cplusplus
}

#include <type_traits>

static_assert(std::is_standard_layout_v<ibmw_cli_command_v1_t>);
static_assert(std::is_trivially_copyable_v<ibmw_cli_command_v1_t>);
static_assert(std::is_standard_layout_v<ibmw_cli_entry_v1_t>);
static_assert(std::is_trivially_copyable_v<ibmw_cli_entry_v1_t>);
#endif
