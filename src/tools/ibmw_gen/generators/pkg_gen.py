#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Pkg pkg_main.cc generator for ibmw-gen.

"""Generate src/<pkg_name>/pkg_main.cc files."""


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


def generate_pkg_mains(project: dict) -> list[tuple[str, str]]:
    """Generate pkg_main.cc files.

    Returns list of (rel_path, content) tuples.
    """
    ns = project["project"]["_namespace_cpp"]
    results = []

    for app in project.get("apps", []):
        pkg_name = app["_pkg_name"]
        mod_names = app["modules"]

        # Build includes
        includes = []
        for mod_name in mod_names:
            mod_dir = _module_dir(mod_name)
            includes.append(f'#include "{mod_dir}/{_snake(mod_name)}.h"')
        include_block = "\n".join(includes)

        # Build registration array entries
        entries = []
        for mod_name in mod_names:
            entries.append(
                f'        {{"{mod_name}", []() -> ibmw::Module* {{\n'
                f"           return new {ns}::{mod_name}();\n"
                f"         }}}}"
            )
        entry_block = ",\n".join(entries)

        # Determine description
        app_type = app.get("type", "pubsub")
        desc = f"Pkg-mode entry point for {app['_snake_name']}."

        content = f"""{_COPYRIGHT}//
// {desc}

#include "pkg/pkg_macro.h"

{include_block}

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{{
{entry_block}}};

IBMW_PKG_MAIN(ibmw_module_register_array)
"""

        results.append((f"src/{pkg_name}/pkg_main.cc", content))

    return results
