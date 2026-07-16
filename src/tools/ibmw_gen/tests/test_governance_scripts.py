#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
"""Pytest bridge for the standalone governance fixture tests."""

import subprocess
from pathlib import Path


def _run_harness(name: str, expected_summary: str) -> None:
    repo_root = Path(__file__).resolve().parents[4]
    harness = repo_root / "scripts" / "tests" / name
    result = subprocess.run(
        ["bash", str(harness)],
        cwd=repo_root,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert result.returncode == 0, (
        f"{name} failed (exit={result.returncode})\n"
        f"--- stdout ---\n{result.stdout}\n--- stderr ---\n{result.stderr}"
    )
    assert expected_summary in result.stdout


def test_docs_governance_harness() -> None:
    _run_harness(
        "test_docs_governance.sh",
        "PASS: docs governance authorities, links, private references, deletion, and rename fixtures",
    )


def test_commit_msg_hook_harness() -> None:
    _run_harness(
        "test_commit_msg_hook.sh",
        "PASS: commit-msg valid, exempt, missing-body, subject, and length fixtures",
    )
