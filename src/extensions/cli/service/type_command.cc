// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_command.h"

#include <iostream>
#include <string>

#include "discovery_options.h"
#include "discovery_query.h"

namespace ibmw::cli::service {
namespace {

int RunType(const Invocation& invocation) {
  const auto snapshot = DiscoverServices(invocation);
  const auto services = VisibleServices(snapshot, invocation.HasOption("--include-hidden-services"));
  const auto& name = invocation.positional_args.front();
  const auto found = services.find(name);
  if (found == services.end()) {
    if (snapshot.Succeeded()) {
      std::cerr << "ibmw service: error: Unknown service '" << name << "'.\n";
    }
    return 1;
  }
  for (const auto& type : found->second) std::cout << type << '\n';
  return snapshot.Succeeded() ? 0 : 1;
}

}  // namespace

void AddTypeCommand(Command& service) {
  auto& type = service.AddSubcommand("type", "Print a discovered service's types");
  type.SetHandler(RunType);
  type.AddArgument("<service>", "Service name exactly as shown by list");
  AddDiscoveryOptions(type);
}

}  // namespace ibmw::cli::service
