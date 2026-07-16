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

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

using namespace ibmw::runtime::core;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {{
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {{
    global_core_ptr->Shutdown();
    return;
  }}
  raise(sig);
}}

int32_t main(int32_t argc, char** argv) {{
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "{app_name} start." << std::endl;

  try {{
    Engine core;
    global_core_ptr = &core;

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
    global_core_ptr = nullptr;
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
    framework's ModuleBus::BroadcastDriverSlot runs after Engine::Initialize()
    and dispatches every used transport driver into every directly-registered
    module via Module::SetDriverSlot(slot_id, ptr). Loaned modules generated
    by ibmw-gen use the IBMW_USE_DRIVER macro to opt in to the typed driver
    they need — identical behaviour to pkg mode. No #include of
    *_transport_driver.h is needed here.
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
// This main.cc contains no driver-injection boilerplate even when
// loaned_publisher modules are present. The framework's
// ModuleBus::BroadcastDriverSlot() runs between Initialize() and Start()
// and dispatches each used transport driver pointer into every registered
// module via Module::SetDriverSlot(slot_id, ptr). Loaned modules use
// IBMW_USE_DRIVER to capture the iceoryx / dds driver pointer they need
// (compile-time typed slot match, no string compare).

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
{include_block}

using namespace ibmw::runtime::core;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {{
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {{
    global_core_ptr->Shutdown();
    return;
  }}
  raise(sig);
}}

int32_t main(int32_t argc, char** argv) {{
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "{app_name} start." << std::endl;

  try {{
    Engine core;
    global_core_ptr = &core;

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

    // Start (blocking). The framework auto-injects transport drivers into
    // any module that overrides Module::SetDriverSlot (typically via the
    // IBMW_USE_DRIVER macro) — no boilerplate needed.
    std::cout << "Starting engine..." << std::endl;
    core.Start();

    // Shutdown
    core.Shutdown();
    global_core_ptr = nullptr;
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
