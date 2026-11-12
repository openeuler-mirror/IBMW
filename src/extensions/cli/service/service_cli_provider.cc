// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "service_command.h"

#include "extension_api/ibmw_cli_provider.h"

namespace {

constexpr char kServiceCommandName[] = "service";
constexpr char kServiceCommandDescription[] = "Inspect and call services";

const ibmw_cli_command_v1_t kCliCommands[] = {{
    .name = {kServiceCommandName, sizeof(kServiceCommandName) - 1},
    .description = {kServiceCommandDescription, sizeof(kServiceCommandDescription) - 1},
    .invoke = &RunServiceCommand,
}};

}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1() {
  return ibmw_cli_entry_v1_t{
      .abi_version = kIbmwCliAbiVersionV1,
      .struct_size = sizeof(ibmw_cli_entry_v1_t),
      .provider_name = {kServiceCommandName, sizeof(kServiceCommandName) - 1},
      .command_count = 1,
      .commands = kCliCommands,
  };
}
