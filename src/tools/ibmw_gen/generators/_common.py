#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Shared helpers used by ibmw_gen code generators and transport plugins.
#
# Extracted from module_gen.py during W4.x deepening so transport
# plugins (transports/<name>.py) can use them without circular
# imports back into module_gen.

"""Common helpers: snake_case conversion + copyright header."""

import os


def output_file_path(output_dir: str, rel_path: str) -> str:
    """Join a generated file's relative path to the output directory.

    Names from the yaml file are part of rel_path: a path that leads out of
    output_dir (e.g. "../x" or an absolute path) is refused.
    """
    abs_path = os.path.join(output_dir, rel_path)
    real_dir = os.path.realpath(output_dir)
    if os.path.commonpath([real_dir, os.path.realpath(abs_path)]) != real_dir:
        raise ValueError(
            f"generated file {rel_path!r} is outside of the output directory "
            f"{output_dir!r}, check the names in your yaml file")
    return abs_path


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
