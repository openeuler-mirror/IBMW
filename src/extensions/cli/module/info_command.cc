// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "info_command.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/runtime_graph/runtime_registry.h"

namespace ibmw::cli::module {
namespace {

using runtime::core::runtime_graph::RuntimeRegistration;

void PrintTopics(const RuntimeRegistration& runtime, const std::string& module_name,
                 bool published) {
  // Report interfaces, not endpoint counts: repeated registrations with the
  // same topic and type appear once, while different types remain distinct.
  std::set<std::pair<std::string, std::string>> topics;
  for (const auto& endpoint : runtime.topics) {
    if (endpoint.module_name == module_name && endpoint.published == published) {
      topics.emplace(endpoint.topic.topic, endpoint.topic.msg_type);
    }
  }
  std::cout << (published ? "Publishers:\n" : "Subscribers:\n");
  if (topics.empty()) std::cout << "  (none)\n";
  for (const auto& [topic, type] : topics) {
    std::cout << "  " << topic << " [" << type << "]\n";
  }
}

int RunModuleInfoCommandImpl(const Invocation& invocation) {
  for (const auto& [name, values] : invocation.options) {
    if (values.size() != 1 || values.front().empty()) {
      throw std::invalid_argument(name + " requires exactly one nonempty value.");
    }
  }
  const auto& module_name = invocation.positional_args.front();
  const auto selected = invocation.OptionValue("--runtime");
  const runtime::core::runtime_graph::RuntimeRegistry registry(
      invocation.OptionValue("--registry-path", extension::DefaultRuntimeRegistryPath().string()));
  const auto runtimes = registry.DiscoverLocalRuntimes();
  bool found_runtime = selected.empty();
  bool complete = true;
  std::vector<const RuntimeRegistration*> matches;
  for (const auto& runtime : runtimes) {
    if (!selected.empty() && runtime.runtime_id != selected) continue;
    found_runtime = true;
    if (!runtime.modules) {
      std::cerr << "ibmw module: runtime '" << runtime.runtime_id
                << "' does not advertise module information; upgrade and restart it.\n";
      complete = false;
      continue;
    }
    if (std::find(runtime.modules->begin(), runtime.modules->end(), module_name) !=
        runtime.modules->end()) {
      matches.push_back(&runtime);
    }
  }
  if (!found_runtime) {
    throw std::runtime_error("Runtime '" + selected + "' was not found in the local registry.");
  }
  if (matches.empty()) {
    if (!complete) return 1;
    throw std::runtime_error("Module '" + module_name + "' was not found in the " +
                             (selected.empty() ? "local registry." : "runtime '" + selected + "'."));
  }
  if (matches.size() > 1) {
    std::string candidates;
    for (const auto* runtime : matches) {
      if (!candidates.empty()) candidates += ", ";
      candidates += "'" + runtime->runtime_id + "'";
    }
    throw std::runtime_error("Module '" + module_name + "' exists in multiple runtimes: " +
                             candidates + ". Select one with --runtime <id>.");
  }
  const auto& runtime = *matches.front();
  std::cout << "Module: " << module_name << '\n'
            << "Runtime: " << runtime.runtime_id << '\n'
            << "PID: " << runtime.pid << "\n\n";
  PrintTopics(runtime, module_name, true);
  std::cout << '\n';
  PrintTopics(runtime, module_name, false);
  return complete ? 0 : 1;
}

}  // namespace

void AddInfoCommand(Command& module) {
  auto& info = module.AddSubcommand("info", "Show a local module's published and subscribed topics");
  info.SetHandler(RunModuleInfoCommandImpl);
  info.AddArgument("<module>", "Module name (use --runtime to select among identical names)");
  info.AddOption("--runtime", {}, "id", "Inspect the module in this runtime",
                 OptionValueMode::kRequired);
  info.AddOption("--registry-path", {}, "path",
                 "Local registry root (default: " + extension::DefaultRuntimeRegistryPath().string() + ")",
                 OptionValueMode::kRequired);
}

}  // namespace ibmw::cli::module
