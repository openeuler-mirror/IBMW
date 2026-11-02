#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-022: names from the yaml file are part of generated paths; none may
# lead out of the output directory.

import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))

from generators._common import output_file_path  # noqa: E402

IBMW_GEN = ROOT / "ibmw_gen.py"
QUICKSTART_YAML = ROOT.parent.parent / "samples" / "cpp" / "quickstart" / "quickstart.ibmw.yaml"


class OutputPathGuardTest(unittest.TestCase):
    def test_paths_inside_output_dir_are_joined(self):
        self.assertEqual(output_file_path("/out", "src/app/main.cc"), "/out/src/app/main.cc")
        self.assertEqual(output_file_path("/out", "src/a/../b/x.h"), "/out/src/a/../b/x.h")

    def test_paths_leaving_output_dir_are_refused(self):
        for rel_path in ("../x.cc", "src/../../x.cc", "/etc/x.cc"):
            with self.assertRaises(ValueError, msg=rel_path):
                output_file_path("/out", rel_path)

    def test_app_name_cannot_write_outside(self):
        with tempfile.TemporaryDirectory() as tmp:
            sandbox = pathlib.Path(tmp)
            yaml_path = sandbox / "evil.ibmw.yaml"
            yaml_path.write_text(QUICKSTART_YAML.read_text().replace(
                "name: helloworld_app", "name: ../../../escaped_app"))
            out = sandbox / "a" / "b" / "out"
            result = subprocess.run([sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out)],
                                    capture_output=True, text=True, timeout=60)
            outside = [p for p in sandbox.rglob("*")
                       if p.is_file() and out not in p.parents and p != yaml_path]
            self.assertEqual(outside, [], result.stdout + result.stderr)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("outside of the output directory", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
