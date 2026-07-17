// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "engine/engine.h"
#include "engine/core_types.h"

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

void PrintUsage() {
  std::cout
      << "OVERVIEW: ibmw — high-performance middleware framework for modern robotics.\n\n"
         "VERSION: "
      << util::GetIbmwVersion()
      << "\n\n"
         "USAGE:\n"
         "  ibmw run <config.yaml> [options]    Start the runtime with the given config\n"
         "  ibmw version                        Show version\n"
         "  ibmw help                           Show this help\n"
         "\n"
         "RUN OPTIONS:\n"
         "  --dump-cfg[=<path>]                 Dump resolved config (default: ./dump_cfg.yaml)\n"
         "  --dump-report[=<path>]              Dump initialization report (default: ./init_report.txt)\n"
         "  --no-signal                         Do not register SIGINT/SIGTERM handlers\n"
         "  --duration=<seconds>                Run for N seconds then exit (default: 0 = forever)\n";
}

// Simple option helpers — no external dependency required.

static bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static std::string OptionValue(const std::string& arg, const std::string& prefix) {
  // --flag=value  → value
  if (arg.size() > prefix.size() && arg[prefix.size()] == '=')
    return arg.substr(prefix.size() + 1);
  return {};
}

int32_t main(int32_t argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 0;
  }

  std::string subcmd = argv[1];

  // ---- ibmw version ----
  if (subcmd == "version" || subcmd == "--version" || subcmd == "-v") {
    PrintVersion();
    return 0;
  }

  // ---- ibmw help ----
  if (subcmd == "help" || subcmd == "--help" || subcmd == "-h") {
    PrintUsage();
    return 0;
  }

  // ---- ibmw run <config.yaml> [options] ----
  if (subcmd != "run") {
    std::cerr << "Unknown command: " << subcmd << "\n\n";
    PrintUsage();
    return 1;
  }

  if (argc < 3) {
    std::cerr << "Error: 'ibmw run' requires a config file path.\n\n"
                 "Usage: ibmw run <config.yaml> [options]\n";
    return 1;
  }

  std::string cfg_file_path = argv[2];

  // Defaults
  bool register_signal = true;
  int32_t running_duration = 0;
  bool dump_cfg = false;
  std::string dump_cfg_path = "./dump_cfg.yaml";
  bool dump_report = false;
  std::string dump_report_path = "./init_report.txt";

  for (int i = 3; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--no-signal") {
      register_signal = false;
    } else if (StartsWith(arg, "--duration")) {
      auto val = OptionValue(arg, "--duration");
      if (val.empty() && i + 1 < argc) {
        val = argv[++i];
      }
      running_duration = std::stoi(val);
    } else if (StartsWith(arg, "--dump-cfg")) {
      dump_cfg = true;
      auto val = OptionValue(arg, "--dump-cfg");
      if (!val.empty()) dump_cfg_path = val;
    } else if (StartsWith(arg, "--dump-report")) {
      dump_report = true;
      auto val = OptionValue(arg, "--dump-report");
      if (!val.empty()) dump_report_path = val;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      return 1;
    }
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
