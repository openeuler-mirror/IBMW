// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <string_view>

#include "extension_api/ibmw_cli_provider.h"

namespace {

int Invoke(int argc, const char* const* argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help" &&
      (std::string_view(argv[0]) == "bag" ||
       std::string_view(argv[0]) == "inspect")) {
    return 17;
  }
  if (argc == 4 && std::string_view(argv[0]) == "bag") {
    return std::string_view(argv[1]) == "record" &&
                   std::string_view(argv[2]) == "--help" &&
                   std::string_view(argv[3]) == "two words" ? 17 : 19;
  }
  return argc == 3 && std::string_view(argv[0]) == "inspect" &&
                 std::string_view(argv[1]) == "--help" &&
                 std::string_view(argv[2]) == "two words"
             ? 17
             : 19;
}

const ibmw_cli_command_v1_t kCommands[] = {
    {{"inspect", 7}, {"Fixture command", 15}, &Invoke},
    {{"bag", 3}, {"Bag fixture", 11}, &Invoke}};

}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw_cli_entry_v1_t IbmwCliEntryV1() {
  return {kIbmwCliAbiVersionV1, sizeof(ibmw_cli_entry_v1_t),
          {"fixture", 7}, 2, kCommands};
}
