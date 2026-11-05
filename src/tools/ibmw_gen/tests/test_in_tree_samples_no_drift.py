#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Every *_gen directory kept in the tree must be what ibmw-gen generates today.
# A difference means the generator or a sample it renders from changed without
# running scripts/regen_in_tree.sh, or a generated file was edited by hand.
#
# Usage:
#   python3 -m pytest tests/test_in_tree_samples_no_drift.py -v
# In a work tree, untracked files inside a *_gen directory count as drift;
# scripts/ci_check.sh runs it on the committed tree.

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
IBMW_GEN = ROOT / "ibmw_gen.py"
SAMPLES = ROOT.parent.parent / "samples" / "cpp"


def _gen_dir_name(sample_yaml: pathlib.Path) -> str:
    # The same rule as scripts/regen_in_tree.sh.
    name = yaml.safe_load(sample_yaml.read_text(encoding="utf-8"))["project"]["name"]
    return name if name.endswith("_gen") else f"{name}_gen"


def _files(directory: pathlib.Path) -> dict[str, pathlib.Path]:
    return {
        path.relative_to(directory).as_posix(): path
        for path in directory.rglob("*")
        if not path.is_dir() and "__pycache__" not in path.parts
    }


def _differences(in_tree: pathlib.Path, generated: pathlib.Path) -> list[str]:
    left, right = _files(in_tree), _files(generated)
    found = [f"only in tree: {name}" for name in sorted(left.keys() - right.keys())]
    found += [f"only generated: {name}" for name in sorted(right.keys() - left.keys())]
    found += [
        f"differs: {name}"
        for name in sorted(left.keys() & right.keys())
        if left[name].read_bytes() != right[name].read_bytes()
    ]
    return found


class InTreeSamplesNoDrift(unittest.TestCase):
    def test_kept_gen_directories_match_the_generator(self) -> None:
        compared = set()
        for sample_yaml in sorted(SAMPLES.glob("*/*.ibmw.yaml")):
            in_tree = sample_yaml.parent / _gen_dir_name(sample_yaml)
            if not in_tree.is_dir():
                continue
            with self.subTest(sample=sample_yaml.name):
                tmp = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_drift_"))
                try:
                    out = tmp / in_tree.name
                    result = subprocess.run(
                        [sys.executable, str(IBMW_GEN), str(sample_yaml), "-o", str(out), "--force"],
                        capture_output=True,
                        text=True,
                        timeout=120,
                    )
                    self.assertEqual(result.returncode, 0,
                                     f"stdout={result.stdout}\nstderr={result.stderr}")
                    self.assertEqual(_differences(in_tree, out), [])
                finally:
                    shutil.rmtree(tmp, ignore_errors=True)
            compared.add(in_tree)
        self.assertTrue(compared)
        # A directory no yaml generates any more would go stale unnoticed.
        self.assertEqual({p for p in SAMPLES.glob("*/*_gen") if p.is_dir()} - compared, set())


if __name__ == "__main__":
    unittest.main()
