// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "module_command.h"

#include "extension_api/ibmw_cli_provider.h"

namespace {

constexpr char kModuleCommandName[] = "module";
constexpr char kModuleCommandDescription[] = "Inspect registered local modules";

const ibmw_cli_command_v1_t kCliCommands[] = {{
    .name = {kModuleCommandName, sizeof(kModuleCommandName) - 1},
    .description = {kModuleCommandDescription, sizeof(kModuleCommandDescription) - 1},
    .invoke = &RunModuleCommand,
}};

}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1() {
  return ibmw_cli_entry_v1_t{
      .abi_version = kIbmwCliAbiVersionV1,
      .struct_size = sizeof(ibmw_cli_entry_v1_t),
      .provider_name = {kModuleCommandName, sizeof(kModuleCommandName) - 1},
      .command_count = 1,
      .commands = kCliCommands,
  };
}
