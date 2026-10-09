// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "module_command.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "cli/command.h"
#include "list_command.h"

int RunModuleCommand(int argc, const char* const* argv) {
  try {
    ibmw::cli::Command module("ibmw module", "Inspect registered local modules");
    ibmw::cli::module::AddListCommand(module);
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    std::vector<char*> mutable_argv;
    for (auto& argument : arguments) mutable_argv.push_back(argument.data());
    return module.Run(static_cast<int>(mutable_argv.size()), mutable_argv.data());
  } catch (const std::exception& error) {
    std::cerr << "ibmw module: error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "ibmw module: error: Unknown failure.\n";
  }
  return 1;
}
