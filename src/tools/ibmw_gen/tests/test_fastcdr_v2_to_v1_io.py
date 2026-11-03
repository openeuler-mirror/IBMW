#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-027: cmake/fastcdr_v2_to_v1.py fails on a missing input before changing
# anything, and replaces each file atomically.
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[4] / "cmake" / "fastcdr_v2_to_v1.py"

HPP = '#include <fastcdr/xcdr/fixed_size_string.hpp>\nstruct A {};\n'


def _run(cwd, *files):
    return subprocess.run([sys.executable, str(SCRIPT), *files], cwd=str(cwd),
                          capture_output=True, text=True, timeout=60)


def test_missing_input_fails_before_any_change(tmp_path):
    (tmp_path / "A.hpp").write_text(HPP)
    result = _run(tmp_path, "A.hpp", "Missing.hpp")
    assert result.returncode == 1
    assert "Missing.hpp not found" in result.stderr
    assert (tmp_path / "A.hpp").read_text() == HPP


def test_transform_leaves_no_temporary_file(tmp_path):
    path = tmp_path / "A.hpp"
    path.write_text(HPP)
    path.chmod(0o640)
    result = _run(tmp_path, "A.hpp")
    assert result.returncode == 0, result.stderr
    assert (path.stat().st_mode & 0o777) == 0o640
    assert [p.name for p in tmp_path.iterdir()] == ["A.hpp"]


def test_interrupted_write_leaves_the_file_unchanged(tmp_path, monkeypatch):
    sys.path.insert(0, str(SCRIPT.parent))
    import fastcdr_v2_to_v1

    path = tmp_path / "A.hpp"
    path.write_text(HPP)

    def fail(_src, _dst):
        raise OSError("interrupted")

    monkeypatch.setattr(fastcdr_v2_to_v1.os, "replace", fail)
    try:
        fastcdr_v2_to_v1.write_atomic(str(path), "x" * 4096)
    except OSError:
        pass
    assert path.read_text() == HPP
    assert [p.name for p in tmp_path.iterdir()] == ["A.hpp"]
