// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "list_command.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "engine/runtime_graph/runtime_registry.h"

namespace ibmw::cli::module {
namespace {

int RunModuleListCommandImpl(const Invocation& invocation) {
  for (const auto& [name, values] : invocation.options) {
    if (values.size() != 1) {
      throw std::invalid_argument(name + " must be specified exactly once.");
    }
    if ((name == "--runtime" || name == "--registry-path") && values.front().empty()) {
      throw std::invalid_argument(name + " requires a nonempty value.");
    }
  }
  const auto selected = invocation.OptionValue("--runtime");
  const runtime::core::runtime_graph::RuntimeRegistry registry(
      invocation.OptionValue("--registry-path", extension::DefaultRuntimeRegistryPath().string()));
  const auto runtimes = registry.DiscoverLocalRuntimes();
  bool found = selected.empty();
  bool complete = true;
  size_t count = 0;
  for (const auto& runtime : runtimes) {
    if (!selected.empty() && runtime.runtime_id != selected) continue;
    found = true;
    if (!runtime.modules) {
      std::cerr << "ibmw module: runtime '" << runtime.runtime_id
                << "' does not advertise module information; upgrade and restart it.\n";
      complete = false;
      continue;
    }
    count += runtime.modules->size();
    if (invocation.HasOption("--count-modules")) continue;
    for (const auto& name : *runtime.modules) {
      std::cout << runtime.runtime_id << '/' << name
                << " [pid: " << runtime.pid << "]\n";
    }
  }
  if (!found) {
    throw std::runtime_error("Runtime '" + selected + "' was not found in the local registry.");
  }
  if (invocation.HasOption("--count-modules")) std::cout << count << '\n';
  return complete ? 0 : 1;
}

}  // namespace

void AddListCommand(Command& module) {
  auto& list = module.AddSubcommand("list", "List registered modules in local runtimes");
  list.SetHandler(RunModuleListCommandImpl);
  list.AddOption("--runtime", {}, "id", "Only list modules in this runtime",
                 OptionValueMode::kRequired);
  list.AddOption("--registry-path", {}, "path",
                 "Local registry root (default: " + extension::DefaultRuntimeRegistryPath().string() + ")",
                 OptionValueMode::kRequired);
  list.AddOption("--count-modules", "-c", {}, "Only display the number of module instances");
}

}  // namespace ibmw::cli::module
