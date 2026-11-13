// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include "engine/engine.h"
#include "engine/core_types.h"
#include "cli_provider_loader.h"
#include "cli/command.h"

using namespace ibmw::runtime::core;

namespace {

// SIGINT/SIGTERM only write the signal number to a self-pipe (async-signal
// safe). A watcher thread turns that into Engine::Shutdown() (logging,
// promises, full teardown on a second signal), so it never runs in
// async-signal context. No signal mask is changed, so threads created earlier
// and child processes keep the default signal behaviour.
std::mutex g_core_mutex;
Engine* global_core_ptr = nullptr;
int g_signal_pipe[2] = {-1, -1};

class ScopedSignalCore {
 public:
  explicit ScopedSignalCore(Engine* core) { Set(core); }
  ~ScopedSignalCore() { Set(nullptr); }  // also on exception unwind
  ScopedSignalCore(const ScopedSignalCore&) = delete;
  ScopedSignalCore& operator=(const ScopedSignalCore&) = delete;

 private:
  static void Set(Engine* core) {
    std::lock_guard<std::mutex> lck(g_core_mutex);
    global_core_ptr = core;
  }
};

void OnShutdownSignal(int sig) {
  const int saved_errno = errno;
  const unsigned char byte = static_cast<unsigned char>(sig);
  // Non-blocking: if the pipe is full a shutdown request is already pending.
  [[maybe_unused]] auto ret = write(g_signal_pipe[1], &byte, 1);
  errno = saved_errno;
}

// No engine (yet / anymore): die from the signal like the default disposition
// would, so the parent sees WIFSIGNALED.
[[noreturn]] void DieFromSignal(int sig) {
  std::signal(sig, SIG_DFL);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
  raise(sig);
  _exit(128 + sig);
}

bool StartSignalThread() {
  if (pipe2(g_signal_pipe, O_CLOEXEC) != 0) return false;
  const int flags = fcntl(g_signal_pipe[1], F_GETFL);
  if (flags < 0 || fcntl(g_signal_pipe[1], F_SETFL, flags | O_NONBLOCK) != 0) return false;

  try {
    std::thread([] {
      while (true) {
        unsigned char byte = 0;
        const ssize_t n = read(g_signal_pipe[0], &byte, 1);
        if (n != 1) {
          if (n < 0 && errno == EINTR) continue;
          return;
        }
        std::lock_guard<std::mutex> lck(g_core_mutex);
        if (global_core_ptr == nullptr) DieFromSignal(byte);
        global_core_ptr->Shutdown();
      }
    }).detach();
  } catch (const std::exception&) {
    return false;
  }

  struct sigaction sa {};
  sa.sa_handler = &OnShutdownSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  return sigaction(SIGINT, &sa, nullptr) == 0 && sigaction(SIGTERM, &sa, nullptr) == 0;
}

// Accepts a whole non-negative number of seconds.
bool ParseDuration(std::string_view text, int32_t& seconds) {
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, seconds);
  return ec == std::errc() && ptr == end && seconds >= 0;
}

template <typename T>
void DumpToFile(const std::string& path, const T& content) {
  std::ofstream ofs(path, std::ios::trunc);
  ofs << content;
  ofs.close();
  if (!ofs) throw std::runtime_error("failed to write " + path);
}

// _exit() skips static destructors (see RunRuntimeCommand) and stdio flushing,
// so flush first.
[[noreturn]] void FlushAndExit(int code) {
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  _exit(code);
}

}  // namespace

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

  if (!ParseDuration(invocation.OptionValue("--duration", "0"), running_duration)) {
    std::cerr << "Error: --duration requires a non-negative integer number of seconds.\n";
    return 1;
  }

  if (register_signal && !StartSignalThread()) {
    std::cerr << "Error: failed to set up SIGINT/SIGTERM handling.\n";
    return 1;
  }

  std::cout << "IBMW v" << util::GetIbmwVersion() << " starting..." << std::endl;

  try {
    Engine core;
    ScopedSignalCore signal_core(&core);

    Engine::Options options;
    options.cfg_file_path = cfg_file_path;

    core.Initialize(options);

    if (dump_cfg) DumpToFile(dump_cfg_path, core.GetConfigSubsystem().GetRootOptionsNode());

    if (dump_report) DumpToFile(dump_report_path, core.GenInitializationReport());

    if (running_duration == 0) {
      core.Start();  // Blocked here
    } else {
      auto fu = core.AsyncStart();
      std::this_thread::sleep_for(std::chrono::seconds(running_duration));
      core.Shutdown();
      fu.wait();
    }
  } catch (const std::exception& e) {
    std::cerr << "IBMW fatal: " << e.what() << std::endl;
    // Same exit status as the former `return -1`; static destructors are
    // skipped for the reason given below.
    FlushAndExit(255);
  }

  std::cout << "IBMW stopped." << std::endl;

  // Use _exit() to skip C++ static destructors and atexit handlers.
  // ROS2/FastDDS libraries register static objects whose destructors
  // reference already-freed resources, causing "double free or corruption"
  // crashes during normal process teardown. All managed shutdown has already
  // completed above (Engine destructor ran, all modules stopped).
  FlushAndExit(0);
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
