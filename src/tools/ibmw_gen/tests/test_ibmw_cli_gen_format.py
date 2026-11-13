#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# GEN-002: the generated format.sh is a bash script (pipefail) and was run with
# sh, so `ibmw_cli gen` failed where sh is dash; a missing formatter also failed
# the whole generation. Both cases now generate the workspace and exit 0.
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

CLI_DIR = Path(__file__).resolve().parents[2] / "ibmw_cli"
TOOLS = ("bash", "find", "xargs")

pytestmark = pytest.mark.skipif(any(shutil.which(t) is None for t in TOOLS), reason="bash/find/xargs missing")


def _path_dir(tmp_path, fakes=()):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for tool in TOOLS:
        (bin_dir / tool).symlink_to(shutil.which(tool))
    log = tmp_path / "calls.log"
    for fake in fakes:
        script = bin_dir / fake
        script.write_text(f'#!{shutil.which("bash")}\necho "{fake} $#" >> "{log}"\n')
        script.chmod(0o755)
    return bin_dir, log


def _generate(tmp_path, bin_dir):
    out = tmp_path / "ws"
    env = {**os.environ, "PATH": str(bin_dir)}
    result = subprocess.run([sys.executable, "-m", "ibmw_cli.main", "gen", "-p", "ibmw_cli/configure_example.yaml",
                             "-o", str(out)], cwd=CLI_DIR, env=env, capture_output=True, text=True, timeout=120)
    return result, out


def test_generation_succeeds_without_formatters(tmp_path):
    bin_dir, _ = _path_dir(tmp_path)
    result, out = _generate(tmp_path, bin_dir)
    assert result.returncode == 0, result.stderr
    for tool in ("clang-format", "cmake-format", "autopep8"):
        assert f"{tool} not found, skipped" in result.stdout
    assert (out / "CMakeLists.txt").is_file() and (out / "format.sh").is_file()


def test_installed_formatters_are_run(tmp_path):
    bin_dir, log = _path_dir(tmp_path, fakes=("clang-format", "cmake-format", "autopep8"))
    result, _ = _generate(tmp_path, bin_dir)
    assert result.returncode == 0, result.stderr
    called = {line.split()[0] for line in log.read_text().splitlines()}
    assert called == {"clang-format", "cmake-format", "autopep8"}
    assert "python format done" in result.stdout
