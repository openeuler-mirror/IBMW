#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-021: ibmw_cli runtime requirements exclude packing/formatting tools, and
# constraints.txt bounds every requirement.
import os
from pathlib import Path

import setuptools  # noqa: F401  # provides distutils on Python >= 3.12
from distutils.core import run_setup  # noqa: E402
from packaging.requirements import Requirement  # noqa: E402

CLI_DIR = Path(__file__).resolve().parents[2] / "ibmw_cli"


def _setup_metadata():
    cwd = os.getcwd()
    os.chdir(CLI_DIR)
    try:
        return run_setup("setup.py", stop_after="init")
    finally:
        os.chdir(cwd)


def _names(lines):
    return {Requirement(line).name.lower() for line in lines}


def _requirement_lines(path):
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")]


def test_runtime_requirements_exclude_build_and_format_tools():
    dist = _setup_metadata()
    assert _names(dist.install_requires) == {"pyyaml", "jinja2"}
    assert _names(dist.extras_require["build"]) == {"pyinstaller"}
    assert _names(dist.extras_require["format"]) == {"autopep8"}
    assert _names(_requirement_lines(CLI_DIR / "ibmw_cli" / "requirements.txt")) == {
        "pyyaml", "jinja2", "setuptools"}


def test_constraints_cover_every_requirement_without_raising_floors():
    dist = _setup_metadata()
    declared = list(dist.install_requires) + [
        line for extra in dist.extras_require.values() for line in extra]
    declared += _requirement_lines(CLI_DIR / "ibmw_cli" / "requirements.txt")
    constraints = {Requirement(line).name.lower(): Requirement(line)
                   for line in _requirement_lines(CLI_DIR / "constraints.txt")}
    for line in declared:
        requirement = Requirement(line)
        bound = constraints[requirement.name.lower()]
        floor = next(spec.version for spec in requirement.specifier if spec.operator == ">=")
        assert bound.specifier.contains(floor), (line, str(bound))
