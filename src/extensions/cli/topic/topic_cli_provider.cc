// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "topic_command.h"

#include "extension_api/ibmw_cli_provider.h"

namespace {

constexpr char kTopicCommandName[] = "topic";
constexpr char kTopicCommandDescription[] = "Inspect discovered topics";
constexpr char kTopicProviderName[] = "topic";

const ibmw_cli_command_v1_t kCliCommands[] = {{
    .name = {kTopicCommandName, sizeof(kTopicCommandName) - 1},
    .description = {kTopicCommandDescription,
                    sizeof(kTopicCommandDescription) - 1},
    .invoke = &RunTopicCommand,
}};

}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1() {
  return ibmw_cli_entry_v1_t{
      .abi_version = kIbmwCliAbiVersionV1,
      .struct_size = sizeof(ibmw_cli_entry_v1_t),
      .provider_name = {kTopicProviderName, sizeof(kTopicProviderName) - 1},
      .command_count = 1,
      .commands = kCliCommands,
  };
}
