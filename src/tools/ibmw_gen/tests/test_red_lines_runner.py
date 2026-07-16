#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Pytest wrapper for the red-line synthetic harnesses (Gate 1).
# Each wrapper runs a sibling shell harness as a subprocess; non-zero exit
# fails the test. The harnesses own the assertions; these wrappers exist
# solely so the synthetic tests count in the Gate 1 pytest total.
import subprocess
from pathlib import Path


def _run_harness(harness_name: str) -> None:
    repo_root = Path(__file__).resolve().parents[4]
    harness = repo_root / "scripts" / "tests" / harness_name
    assert harness.is_file(), f"harness not found: {harness}"
    result = subprocess.run(
        ["bash", str(harness)],
        cwd=str(repo_root),
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert result.returncode == 0, (
        f"{harness_name} failed (exit={result.returncode})\n"
        f"--- stdout ---\n{result.stdout}\n--- stderr ---\n{result.stderr}"
    )


def test_red_lines_r8_r12_synthetic_harness():
    _run_harness("test_red_lines_r8_r12.sh")


def test_red_lines_exempt_anchors_synthetic_harness():
    _run_harness("test_red_lines_exempt_anchors.sh")


def test_target_ros2_prefix_synthetic_harness():
    _run_harness("test_target_ros2_prefix.sh")
