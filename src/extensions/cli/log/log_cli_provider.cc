// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_command.h"
#include "extension_api/ibmw_cli_provider.h"

namespace {
constexpr char kName[] = "log";
constexpr char kDescription[] = "List and set logger levels through RPC";
const ibmw_cli_command_v1_t kCommands[] = {{
    .name = {kName, sizeof(kName) - 1},
    .description = {kDescription, sizeof(kDescription) - 1},
    .invoke = &RunLogCommand,
}};
}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1() {
  return {.abi_version = kIbmwCliAbiVersionV1,
          .struct_size = sizeof(ibmw_cli_entry_v1_t),
          .provider_name = {kName, sizeof(kName) - 1},
          .command_count = 1,
          .commands = kCommands};
}
