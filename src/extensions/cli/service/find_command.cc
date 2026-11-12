// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "find_command.h"

#include <iostream>

#include "discovery_options.h"
#include "discovery_query.h"

namespace ibmw::cli::service {
namespace {

int RunFind(const Invocation& invocation) {
  const auto snapshot = DiscoverServices(invocation);
  const auto services = VisibleServices(snapshot, invocation.HasOption("--include-hidden-services"));
  const auto& type = invocation.positional_args.front();
  size_t count = 0;
  for (const auto& [name, types] : services) {
    if (!types.contains(type)) continue;
    ++count;
    if (!invocation.HasOption("--count-services")) std::cout << name << '\n';
  }
  if (invocation.HasOption("--count-services")) std::cout << count << '\n';
  return snapshot.Succeeded() ? 0 : 1;
}

}  // namespace

void AddFindCommand(Command& service) {
  auto& find = service.AddSubcommand("find", "Find services by exact type");
  find.SetHandler(RunFind);
  find.AddArgument("<type>", "Exact type from type/list -t (quote request -> response pairs)");
  find.AddOption("--count-services", "-c", {}, "Only display the number of matching service names");
  AddDiscoveryOptions(find);
}

}  // namespace ibmw::cli::service
