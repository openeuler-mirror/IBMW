// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "list_command.h"

#include <iostream>

#include "discovery_options.h"
#include "discovery_query.h"

namespace ibmw::cli::service {
namespace {

int RunList(const Invocation& invocation) {
  const auto snapshot = DiscoverServices(invocation);
  const auto services = VisibleServices(snapshot, invocation.HasOption("--include-hidden-services"));
  if (invocation.HasOption("--count-services")) {
    std::cout << services.size() << '\n';
  } else {
    for (const auto& [name, types] : services) {
      std::cout << name;
      if (invocation.HasOption("--show-types")) {
        std::cout << " [";
        bool first = true;
        for (const auto& type : types) {
          if (!first) std::cout << ", ";
          first = false;
          std::cout << type;
        }
        std::cout << ']';
      }
      std::cout << '\n';
    }
  }
  return snapshot.Succeeded() ? 0 : 1;
}

}  // namespace

void AddListCommand(Command& service) {
  auto& list = service.AddSubcommand("list", "List registered service methods");
  list.SetHandler(RunList);
  list.AddOption("--show-types", "-t", {}, "Additionally show request -> response types");
  list.AddOption("--count-services", "-c", {}, "Only display the number of service names");
  AddDiscoveryOptions(list);
}

}  // namespace ibmw::cli::service
