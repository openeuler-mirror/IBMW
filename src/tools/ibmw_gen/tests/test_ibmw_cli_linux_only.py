#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# HYG-021: IBMW targets Linux and OHOS; ibmw_cli no longer generates Windows
# deploy files or Windows/MSVC branches in workspace CMake files.
import re
import sys
from pathlib import Path

CLI_DIR = Path(__file__).resolve().parents[2] / "ibmw_cli"
sys.path.insert(0, str(CLI_DIR))

from ibmw_cli.generator.deploy_generator import deploy_template_infos  # noqa: E402

TEMPLATES = CLI_DIR / "ibmw_cli" / "templates"
WINDOWS = re.compile(r"\bWIN32\b|MSVC|\"Windows\"|install/win\b|\.bat\b")


def test_deploy_generates_linux_files_only():
    assert [info["template_name"] for info in deploy_template_infos] == ["linux_cfg_yaml", "linux_start_shell"]
    for info in deploy_template_infos:
        assert (TEMPLATES / "install" / info["template_file"]).is_file()


def test_templates_have_no_windows_branches():
    offenders = [str(path.relative_to(TEMPLATES)) for path in TEMPLATES.rglob("*.jinja2")
                 if WINDOWS.search(path.read_text(encoding="utf-8"))]
    assert offenders == []
