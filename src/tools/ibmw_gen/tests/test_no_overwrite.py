#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Smoke test for ibmw-gen "generate once, don't overwrite" behaviour.
#
# Scenario:
#   1. Run ibmw-gen on the quickstart yaml → all files created.
#   2. Inject a SENTINEL into each user-editable file (.h/.cc under src/<module>/).
#   3. Re-run ibmw-gen (no --force) → user files keep the sentinel; auto files are fresh.
#   4. Re-run ibmw-gen --force → all files overwritten, sentinels gone.
#
# Usage:
#   python3 -m pytest tests/test_no_overwrite.py -v
#   # or simply:
#   python3 tests/test_no_overwrite.py

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
IBMW_GEN = ROOT / "ibmw_gen.py"
QUICKSTART_YAML = (
    ROOT.parent.parent / "samples" / "cpp" / "quickstart" / "quickstart.ibmw.yaml"
)

SENTINEL = "/* SMOKE_TEST_USER_EDIT_SENTINEL_12345 */"


def run_gen(yaml_path: pathlib.Path, output_dir: pathlib.Path,
            extra_args: list[str] | None = None) -> subprocess.CompletedProcess:
    cmd = [sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(output_dir)]
    if extra_args:
        cmd.extend(extra_args)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=30)


def _find_user_files(output_dir: pathlib.Path) -> list[pathlib.Path]:
    """Return module .h/.cc files that should be protected from overwrite."""
    user_files = []
    src = output_dir / "src"
    if not src.exists():
        return user_files
    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = str(f.relative_to(output_dir))
        basename = f.name
        # Exclude auto-overwrite files: main.cc, pkg_main.cc, json_*.h
        if basename in ("main.cc", "pkg_main.cc", "json_types.h", "json_custom_types.h"):
            continue
        if basename.endswith("_dds_types.h") or basename.endswith("_cdr_impl.cc"):
            continue
        if basename.endswith(".h") or basename.endswith(".cc"):
            user_files.append(f)
    return sorted(user_files)


def _find_auto_files(output_dir: pathlib.Path) -> list[pathlib.Path]:
    """Return files that ibmw-gen should always overwrite."""
    auto = []
    # CMakeLists.txt
    cml = output_dir / "CMakeLists.txt"
    if cml.exists():
        auto.append(cml)
    # cfg/*.yaml
    cfg = output_dir / "cfg"
    if cfg.exists():
        auto.extend(sorted(cfg.rglob("*.yaml")))
    # protocol/*.h
    proto = output_dir / "protocol"
    if proto.exists():
        auto.extend(sorted(proto.rglob("*.h")))
    # main.cc, pkg_main.cc (anywhere under src/)
    src = output_dir / "src"
    if src.exists():
        for f in src.rglob("main.cc"):
            auto.append(f)
        for f in src.rglob("pkg_main.cc"):
            auto.append(f)
    return sorted(set(auto))


class TestNoOverwrite(unittest.TestCase):
    """Smoke test: ibmw-gen must not overwrite user-edited module files."""

    def setUp(self):
        self.tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_smoke_"))
        self.output_dir = self.tmpdir / "quickstart_gen"

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    # ── Step 1: initial generation ──────────────────────────────────────

    def _generate_initial(self):
        result = run_gen(QUICKSTART_YAML, self.output_dir)
        self.assertEqual(result.returncode, 0,
                         f"ibmw-gen failed:\nstdout={result.stdout}\nstderr={result.stderr}")
        return result

    # ── Helpers ──────────────────────────────────────────────────────────

    def _inject_sentinel(self, files: list[pathlib.Path]):
        for f in files:
            content = f.read_text(encoding="utf-8")
            f.write_text(SENTINEL + "\n" + content, encoding="utf-8")

    def _assert_sentinel_present(self, files: list[pathlib.Path]):
        for f in files:
            content = f.read_text(encoding="utf-8")
            self.assertIn(SENTINEL, content,
                          f"Sentinel MISSING in user file (was overwritten!): {f.name}")

    def _assert_sentinel_absent(self, files: list[pathlib.Path]):
        for f in files:
            content = f.read_text(encoding="utf-8")
            self.assertNotIn(SENTINEL, content,
                             f"Sentinel still present (was NOT overwritten): {f.name}")

    # ── Tests ───────────────────────────────────────────────────────────

    def test_initial_generation_creates_files(self):
        """Step 1: First run creates all expected files."""
        self._generate_initial()

        user_files = _find_user_files(self.output_dir)
        auto_files = _find_auto_files(self.output_dir)

        self.assertGreater(len(user_files), 0, "Expected at least one user-editable file")
        self.assertGreater(len(auto_files), 0, "Expected at least one auto-generated file")

        # CMakeLists.txt must exist
        self.assertTrue((self.output_dir / "CMakeLists.txt").exists())

    def test_rerun_preserves_user_files(self):
        """Steps 2-3: Re-run without --force preserves edited user files."""
        self._generate_initial()

        user_files = _find_user_files(self.output_dir)
        auto_files = _find_auto_files(self.output_dir)
        self.assertGreater(len(user_files), 0)

        # Inject sentinel into user files AND auto files
        self._inject_sentinel(user_files)
        self._inject_sentinel(auto_files)

        # Re-run without --force
        result = run_gen(QUICKSTART_YAML, self.output_dir)
        self.assertEqual(result.returncode, 0,
                         f"Re-run failed:\nstdout={result.stdout}\nstderr={result.stderr}")

        # User files: sentinel must survive
        self._assert_sentinel_present(user_files)

        # Auto files: sentinel must be gone (overwritten)
        self._assert_sentinel_absent(auto_files)

    def test_force_overwrites_everything(self):
        """Step 4: --force overwrites user files too."""
        self._generate_initial()

        user_files = _find_user_files(self.output_dir)
        self.assertGreater(len(user_files), 0)

        self._inject_sentinel(user_files)

        # Re-run WITH --force
        result = run_gen(QUICKSTART_YAML, self.output_dir, extra_args=["--force"])
        self.assertEqual(result.returncode, 0,
                         f"Force run failed:\nstdout={result.stdout}\nstderr={result.stderr}")

        # All sentinels must be gone
        self._assert_sentinel_absent(user_files)

    def test_dry_run_does_not_write(self):
        """--dry-run must not create or modify any files."""
        # Run with --dry-run into a fresh directory
        result = run_gen(QUICKSTART_YAML, self.output_dir, extra_args=["--dry-run"])
        self.assertEqual(result.returncode, 0)

        # Output dir should either not exist or be empty
        if self.output_dir.exists():
            files = list(self.output_dir.rglob("*"))
            files = [f for f in files if f.is_file()]
            self.assertEqual(len(files), 0,
                             f"--dry-run created files: {[str(f) for f in files]}")


if __name__ == "__main__":
    unittest.main()
