#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-024: ros2_gen_ibmw_cpp_rpc.py requires its arguments and keeps an '='
# inside a value, instead of silently writing to the working directory.
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "ros2_gen_rpc" / "ros2_gen_ibmw_cpp_rpc.py"


def _run(cwd, *args):
    return subprocess.run([sys.executable, str(SCRIPT), *args], cwd=str(cwd),
                          capture_output=True, text=True, timeout=60)


def test_missing_output_path_fails_without_writing(tmp_path):
    result = _run(tmp_path, "--pkg_name=pkg", "--srv_file=/x/AddTwoInts.srv")
    assert result.returncode != 0
    assert "--output_path" in result.stderr
    assert list(tmp_path.iterdir()) == []


def test_value_keeps_equal_sign_and_extra_options_are_ignored(tmp_path):
    out = tmp_path / "out"
    out.mkdir()
    result = _run(tmp_path, "--pkg_name=pkg", "--srv_file=/x/a=b.srv", f"--output_path={out}", "--extra=1")
    assert result.returncode == 0, result.stderr
    assert sorted(p.name for p in out.iterdir()) == ["a=b.ibmw_rpc.srv.cc", "a=b.ibmw_rpc.srv.h"]
