#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-023: module values are emitted into C++ string literals and #include
# lines unescaped, so a quote, backslash or control character is refused when
# the yaml is loaded and nothing is generated.

import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
IBMW_GEN = ROOT / "ibmw_gen.py"
PUBSUB_YAML = ROOT.parents[2] / "skills" / "migration" / "templates" / "json_local_pubsub.ibmw.yaml"


class ModuleStringSafetyTest(unittest.TestCase):
    def _generate(self, yaml_text):
        with tempfile.TemporaryDirectory() as tmp:
            yaml_path = pathlib.Path(tmp) / "case.ibmw.yaml"
            yaml_path.write_text(yaml_text)
            out = pathlib.Path(tmp) / "out"
            result = subprocess.run([sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out)],
                                    capture_output=True, text=True, timeout=60)
            generated = [p for p in out.rglob("*") if p.is_file()] if out.exists() else []
            return result, generated

    def test_unchanged_template_generates(self):
        result, generated = self._generate(PUBSUB_YAML.read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(generated)

    def test_topic_that_ends_the_cpp_literal_is_refused(self):
        source = PUBSUB_YAML.read_text()
        for topic in (r'"chat\"); std::abort(); (\""', r'"chat\\"', r'"chat\nter"'):
            with self.subTest(topic=topic):
                case = source.replace('topic: "chatter"', f"topic: {topic}", 1)
                result, generated = self._generate(case)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("Module 'PublisherModule': 'topic' must not contain", result.stderr)
                self.assertEqual(generated, [])

    def test_nested_values_are_checked(self):
        config = '\n    custom_config:\n      greeting: "say \\"hi\\""'
        source = PUBSUB_YAML.read_text().replace('topic: "chatter"', 'topic: "chatter"' + config, 1)
        result, generated = self._generate(source)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("'custom_config.greeting' must not contain", result.stderr)
        self.assertEqual(generated, [])


if __name__ == "__main__":
    unittest.main()
