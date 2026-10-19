// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "bag_command.h"

#include "extension_api/ibmw_cli_provider.h"

namespace {

constexpr char kBagCommandName[] = "bag";
constexpr char kBagCommandDescription[] =
    "Record, play, convert, or inspect bag data";
constexpr char kBagProviderName[] = "bag";

const ibmw_cli_command_v1_t kCliCommands[] = {{
    .name = {kBagCommandName, sizeof(kBagCommandName) - 1},
    .description = {kBagCommandDescription,
                    sizeof(kBagCommandDescription) - 1},
    .invoke = &IbmwRunBagCommand,
}};

}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1() {
  return ibmw_cli_entry_v1_t{
      .abi_version = kIbmwCliAbiVersionV1,
      .struct_size = sizeof(ibmw_cli_entry_v1_t),
      .provider_name = {kBagProviderName,
                        sizeof(kBagProviderName) - 1},
      .command_count = 1,
      .commands = kCliCommands,
  };
}
