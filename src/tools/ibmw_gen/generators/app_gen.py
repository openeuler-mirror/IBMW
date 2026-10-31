#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# App main.cc generator for ibmw-gen.

"""Generate src/<app_name>/main.cc files."""


def _snake(name: str) -> str:
    """Convert PascalCase to snake_case."""
    result = []
    for i, ch in enumerate(name):
        if ch.isupper() and i > 0:
            prev = name[i - 1]
            if prev.islower() or prev.isdigit():
                result.append("_")
            elif i + 1 < len(name) and name[i + 1].islower():
                result.append("_")
        result.append(ch.lower())
    return "".join(result)


_COPYRIGHT = """\
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
"""


# Signal handling of the generated mains. Engine::Shutdown() logs and locks,
# so it must not run in a signal handler: the handler only writes the signal
# number to a pipe and a watcher thread calls Shutdown().
_SIGNAL_SUPPORT = """\
// SIGINT/SIGTERM only write the signal number to a pipe (async-signal-safe);
// a watcher thread calls Engine::Shutdown(), which logs and locks.
std::mutex g_core_mutex;
Engine* global_core_ptr = nullptr;
int g_signal_pipe[2] = {-1, -1};

// Makes `core` the target of SIGINT/SIGTERM while in scope.
class ScopedSignalCore {
 public:
  explicit ScopedSignalCore(Engine* core) { Set(core); }
  ~ScopedSignalCore() { Set(nullptr); }
  ScopedSignalCore(const ScopedSignalCore&) = delete;
  ScopedSignalCore& operator=(const ScopedSignalCore&) = delete;

 private:
  static void Set(Engine* core) {
    std::lock_guard<std::mutex> lck(g_core_mutex);
    global_core_ptr = core;
  }
};

void SignalHandler(int sig) {
  const int saved_errno = errno;
  const unsigned char byte = static_cast<unsigned char>(sig);
  [[maybe_unused]] auto ret = write(g_signal_pipe[1], &byte, 1);
  errno = saved_errno;
}

bool InstallSignalHandlers() {
  if (pipe2(g_signal_pipe, O_CLOEXEC) != 0 ||
      fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK) != 0) {
    return false;
  }
  std::thread([] {
    unsigned char sig = 0;
    while (true) {
      const ssize_t n = read(g_signal_pipe[0], &sig, 1);
      if (n != 1) {
        if (n < 0 && errno == EINTR) continue;
        return;
      }
      std::lock_guard<std::mutex> lck(g_core_mutex);
      if (global_core_ptr == nullptr) {
        // No engine (anymore): die from the signal.
        std::signal(sig, SIG_DFL);
        raise(sig);
        continue;
      }
      global_core_ptr->Shutdown();
    }
  }).detach();
  return std::signal(SIGINT, SignalHandler) != SIG_ERR &&
         std::signal(SIGTERM, SignalHandler) != SIG_ERR;
}
"""


def _module_dir(mod_name: str) -> str:
    """Compute module directory name from module name."""
    snake = _snake(mod_name)
    return snake if snake.endswith("_module") else snake + "_module"


def _gen_bare_app(app: dict, project: dict) -> str:
    """Generate main.cc for bare app (uses CreateModule + AsyncStart)."""
    app_name = app["_snake_name"]
    ns = project["project"]["_namespace_cpp"]

    return f"""{_COPYRIGHT}//
// App-mode entry point for {app_name}.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"

using namespace ibmw::runtime::core;

{_SIGNAL_SUPPORT}
int32_t main(int32_t argc, char** argv) {{
  if (!InstallSignalHandlers()) {{
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return -1;
  }}

  std::cout << "{app_name} start." << std::endl;

  try {{
    Engine core;
    ScopedSignalCore signal_core(&core);

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];
    core.Initialize(options);

    // Create module handle (module name must match YAML config)
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("{app['modules'][0]}"));

    MW_LOG(module_handle.GetLogger(),
           ibmw::common::util::kLogLevelInfo,
           "App mode started successfully.");

    // Start the engine asynchronously
    auto fu = core.AsyncStart();

    MW_LOG(module_handle.GetLogger(),
           ibmw::common::util::kLogLevelInfo,
           "Engine is running. Press Ctrl+C to stop.");

    // Wait for shutdown signal
    fu.wait();

    // Clean shutdown
    core.Shutdown();
  }} catch (const std::exception& e) {{
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return -1;
  }}

  std::cout << "{app_name} exit." << std::endl;
  return 0;
}}
"""


def _gen_standard_app(app: dict, project: dict) -> str:
    """Generate main.cc for standard app (registers modules, Start blocking).

    Standalone main.cc emits no per-app driver-injection boilerplate. The
    Managed modules use the public Runtime and transport access APIs. Concrete
    driver injection is reserved for explicitly direct transport modules.
    """
    app_name = app["_snake_name"]
    ns = project["project"]["_namespace_cpp"]
    module_map = project["_module_map"]
    mod_names = app["modules"]

    # Build includes
    includes = []
    for mod_name in mod_names:
        mod = module_map[mod_name]
        mod_dir = _module_dir(mod_name)
        includes.append(f'#include "{mod_dir}/{_snake(mod_name)}.h"')
    include_block = "\n".join(includes)

    # Build variable declarations and registrations.
    decl_lines = []
    reg_lines = []
    for i, mod_name in enumerate(mod_names):
        var = f"module_{i}"
        decl_lines.append(f"    {ns}::{mod_name} {var};")
        reg_lines.append(f"    core.GetModuleBus().RegisterModule({var}.NativeHandle());")
    decl_block = "\n".join(decl_lines)
    reg_block = "\n".join(reg_lines)

    return f"""{_COPYRIGHT}//
// App-mode entry point for {app_name}.
//
// Managed modules use public transport access handles. Concrete driver
// injection is reserved for explicitly direct transport modules.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"
{include_block}

using namespace ibmw::runtime::core;

{_SIGNAL_SUPPORT}
int32_t main(int32_t argc, char** argv) {{
  if (!InstallSignalHandlers()) {{
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return -1;
  }}

  std::cout << "{app_name} start." << std::endl;

  try {{
    Engine core;
    ScopedSignalCore signal_core(&core);

    // Register modules
{decl_block}

{reg_block}

    // Initialize
    Engine::Options options;
    if (argc > 1) {{
      options.cfg_file_path = argv[1];
    }} else {{
      std::cerr << "Usage: " << argv[0] << " <config.yaml>" << std::endl;
      return -1;
    }}

    core.Initialize(options);

    // Start (blocking). Runtime configuration selects concrete transports.
    std::cout << "Starting engine..." << std::endl;
    core.Start();

    // Shutdown
    core.Shutdown();
  }} catch (const std::exception& e) {{
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return -1;
  }}

  std::cout << "{app_name} exit." << std::endl;
  return 0;
}}
"""


def generate_app_mains(project: dict) -> list[tuple[str, str]]:
    """Generate app main.cc files.

    Returns list of (rel_path, content) tuples.
    """
    results = []

    for app in project.get("apps", []):
        app_name = app["_snake_name"]
        app_type = app.get("type", "pubsub")

        # Bare app: uses CreateModule + AsyncStart
        if app_type == "bare":
            content = _gen_bare_app(app, project)
        else:
            content = _gen_standard_app(app, project)

        results.append((f"src/{app_name}/main.cc", content))

    return results
