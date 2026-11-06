// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_command.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "call_options.h"
#include "cli/command.h"
#include "log_client.h"
#include "log_control_ext/log_control_types.h"

namespace ibmw::cli::log {
namespace {
namespace protocol = ibmw::protocols::log_control_extension;

int List(const Invocation& invocation) {
  const auto levels = QueryLogLevels(invocation, invocation.positional_args);
  for (const auto& [name, value] : levels)
    std::cout << name << ' ' << value << '\n';
  return 0;
}

int Set(const Invocation& invocation) {
  SetLogLevel(invocation, invocation.positional_args[0], invocation.positional_args[1]);
  std::cout << "Set log level successful\n";
  return 0;
}

void AddOptions(Command& command) {
  service::AddCallTransportOptions(command, false);
  command.AddOption("--service", {}, "name",
      "Log control service base name (default: " + std::string(protocol::kServiceName) + ")",
      OptionValueMode::kRequired);
}
}  // namespace
}  // namespace ibmw::cli::log

int RunLogCommand(int argc, const char* const* argv) {
  using namespace ibmw::cli;
  try {
    Command command("ibmw log", "List and set runtime logger levels using one RPC backend; set does not save files");
    auto& list = command.AddSubcommand("list", "List registered logger names and levels in the selected log service, including core");
    list.AddArgument("[module]", "Optional logger/module name", false).SetHandler(log::List);
    log::AddOptions(list);
    auto& set = command.AddSubcommand("set", "Change an existing logger's runtime level (no file writes)");
    set.AddArgument("<module>", "Logger/module name, or core")
       .AddArgument("<level>", "TRACE, DEBUG, INFO, WARN, ERROR, FATAL or OFF; case-insensitive").SetHandler(log::Set);
    log::AddOptions(set);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    std::vector<char*> mutable_args;
    for (auto& arg : args) mutable_args.push_back(arg.data());
    return command.Run(static_cast<int>(mutable_args.size()), mutable_args.data());
  } catch (const log::CallFailure& failure) {
    return failure.exit_code;
  } catch (const std::exception& error) {
    std::cerr << "ibmw log: error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "ibmw log: error: Unknown failure.\n";
  }
  return 1;
}
