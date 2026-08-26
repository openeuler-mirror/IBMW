// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "engine/engine.h"
#include "engine/core_types.h"
#include "cli_provider_loader.h"
#include "cli/command.h"

using namespace ibmw::runtime::core;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }
  raise(sig);
}

void PrintVersion() {
  std::cout << "ibmw " << util::GetIbmwVersion() << std::endl;
}

int RunRuntimeCommand(const ibmw::cli::Invocation& invocation) {
  const std::string cfg_file_path = invocation.positional_args.front();

  // Defaults
  const bool register_signal = !invocation.HasOption("--no-signal");
  int32_t running_duration = 0;
  bool dump_cfg = invocation.HasOption("--dump-cfg");
  std::string dump_cfg_path =
      invocation.OptionValue("--dump-cfg", "./dump_cfg.yaml");
  bool dump_report = invocation.HasOption("--dump-report");
  std::string dump_report_path =
      invocation.OptionValue("--dump-report", "./init_report.txt");

  try {
    running_duration = std::stoi(invocation.OptionValue("--duration", "0"));
  } catch (const std::exception&) {
    std::cerr << "Error: --duration requires an integer number of seconds.\n";
    return 1;
  }

  if (register_signal) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
  }

  std::cout << "IBMW v" << util::GetIbmwVersion() << " starting..." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

    Engine::Options options;
    options.cfg_file_path = cfg_file_path;

    core.Initialize(options);

    if (dump_cfg) {
      std::ofstream ofs(dump_cfg_path, std::ios::trunc);
      ofs << core.GetConfigSubsystem().GetRootOptionsNode();
      ofs.close();
    }

    if (dump_report) {
      std::ofstream ofs(dump_report_path, std::ios::trunc);
      ofs << core.GenInitializationReport();
      ofs.close();
    }

    if (running_duration == 0) {
      core.Start();  // Blocked here
    } else {
      auto fu = core.AsyncStart();
      std::this_thread::sleep_for(std::chrono::seconds(running_duration));
      core.Shutdown();
      fu.wait();
    }

    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "IBMW fatal: " << e.what() << std::endl;
    return -1;
  }

  std::cout << "IBMW stopped." << std::endl;

  // Use _exit() to skip C++ static destructors and atexit handlers.
  // ROS2/FastDDS libraries register static objects whose destructors
  // reference already-freed resources, causing "double free or corruption"
  // crashes during normal process teardown. All managed shutdown has already
  // completed above (Engine destructor ran, all modules stopped).
  _exit(0);
}

int RunCli(int argc, char** argv) {
  std::filesystem::path cli_config_path;
  int command_index = 1;
  if (argc > 1 && std::string_view(argv[1]) == "--cli-config") {
    if (argc > 2) cli_config_path = argv[2];
    command_index = 3;
  } else if (argc > 1 && std::string_view(argv[1]).starts_with("--cli-config=")) {
    cli_config_path = std::string_view(argv[1]).substr(13);
    command_index = 2;
  }
  if (command_index != 1 && (cli_config_path.empty() || argc <= command_index)) {
    std::cerr << "Error: '--cli-config' requires a path and a command.\n";
    return 1;
  }
  std::vector<char*> arguments{argv[0]};
  for (int index = command_index; index < argc; ++index) {
    arguments.push_back(argv[index]);
  }

  ibmw::cli::Command app(
      "ibmw", "High-performance middleware framework for modern robotics.");
  app.AddOption(
      "--cli-config", {}, "path", "CLI provider configuration",
      ibmw::cli::OptionValueMode::kRequired);

  auto& version = app.AddSubcommand("version", "Show version");
  version.AddAlias("--version").AddAlias("-v");
  version.SetHandler([](const ibmw::cli::Invocation&) {
    PrintVersion();
    return 0;
  });

  auto& run = app.AddSubcommand("run", "Start the runtime with a config file");
  run.AddArgument("<config.yaml>", "Runtime YAML configuration");
  run.AddOption(
      "--dump-cfg", {}, "path", "Dump resolved config (default: ./dump_cfg.yaml)",
      ibmw::cli::OptionValueMode::kOptional);
  run.AddOption(
      "--dump-report", {}, "path",
      "Dump initialization report (default: ./init_report.txt)",
      ibmw::cli::OptionValueMode::kOptional);
  run.AddOption(
      "--duration", {}, "seconds", "Run for N seconds (default: 0 = forever)",
      ibmw::cli::OptionValueMode::kRequired);
  run.AddOption(
      "--no-signal", {}, {}, "Do not register SIGINT/SIGTERM handlers");
  run.SetHandler(RunRuntimeCommand);

  const std::string_view command = arguments.size() > 1 ? arguments[1] : "";
  const bool help = command.empty() || command == "help" ||
                    command == "--help" || command == "-h";
  if (!help && !app.HasSubcommand(command)) {
    const auto result = RunCliCommandFromProviders(
        static_cast<int>(arguments.size()) - 1, arguments.data() + 1,
        cli_config_path);
    if (result != kCliCommandNotFound) return result;
  }
  const bool version_command = command == "version" || command == "--version" ||
                               command == "-v";
  if (!help && !version_command && !cli_config_path.empty() &&
      app.HasSubcommand(command)) {
    std::cerr << "Error: '--cli-config' applies only to provider commands.\n";
    return 1;
  }
  return app.Run(static_cast<int>(arguments.size()), arguments.data());
}

#ifndef IBMW_CLI_TEST
int32_t main(int32_t argc, char** argv) {
  return RunCli(argc, argv);
}
#endif
