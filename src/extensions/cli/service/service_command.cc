// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "service_command.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "cli/command.h"
#include "find_command.h"
#include "list_command.h"
#include "type_command.h"

int RunServiceCommand(int argc, const char* const* argv) {
  try {
    ibmw::cli::Command service("ibmw service", "Inspect registered local services");
    service.AddHelpOption("--include-hidden-services",
                          "Include hidden services in list, type and find queries");
    ibmw::cli::service::AddListCommand(service);
    ibmw::cli::service::AddTypeCommand(service);
    ibmw::cli::service::AddFindCommand(service);

    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    // Match topic's parent-level and subcommand-level hidden-name switches.
    bool include_hidden = false;
    while (arguments.size() > 1 && arguments[1] == "--include-hidden-services") {
      include_hidden = true;
      arguments.erase(arguments.begin() + 1);
    }
    if (include_hidden && arguments.size() > 1 && service.HasSubcommand(arguments[1])) {
      arguments.insert(arguments.begin() + 2, "--include-hidden-services");
    }
    std::vector<char*> mutable_argv;
    for (auto& argument : arguments) mutable_argv.push_back(argument.data());
    return service.Run(static_cast<int>(mutable_argv.size()), mutable_argv.data());
  } catch (const std::exception& error) {
    std::cerr << "ibmw service: error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "ibmw service: error: Unknown failure.\n";
  }
  return 1;
}
