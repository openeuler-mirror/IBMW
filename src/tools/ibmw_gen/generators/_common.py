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
