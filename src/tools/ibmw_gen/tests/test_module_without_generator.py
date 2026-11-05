#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-045: the project CMake and configs list every module, so a module the
# generator cannot emit fails the generation instead of leaving a project that
# names missing sources. A ROS2 RPC module needs its service.

import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))
IBMW_GEN = ROOT / "ibmw_gen.py"
ROS2_RPC_YAML = ROOT.parents[1] / "samples" / "cpp" / "ros2_rpc" / "ros2_rpc.ibmw.yaml"

from generators.module_gen import ModuleGenError, generate_modules  # noqa: E402


class ModuleWithoutGeneratorTest(unittest.TestCase):
    def _generate(self, yaml_text):
        with tempfile.TemporaryDirectory() as tmp:
            yaml_path = pathlib.Path(tmp) / "case.ibmw.yaml"
            yaml_path.write_text(yaml_text)
            out = pathlib.Path(tmp) / "out"
            result = subprocess.run([sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out)],
                                    capture_output=True, text=True, timeout=60)
            generated = {p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()} \
                if out.exists() else set()
            return result, generated

    def test_ros2_rpc_with_service_generates_its_sources(self):
        result, generated = self._generate(ROS2_RPC_YAML.read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("src/rpc_server_module/rpc_server_module.cc", generated)
        self.assertIn("src/rpc_client_module/rpc_client_module.cc", generated)

    def test_ros2_rpc_without_service_is_refused(self):
        source = ROS2_RPC_YAML.read_text()
        self.assertEqual(source.count("    message: RosTestRpc\n"), 2)
        result, generated = self._generate(source.replace(
            "    message: RosTestRpc\n", "    ros2_package: example_ros2\n", 1))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Module 'RpcServerModule' (ROS2 RPC) requires 'ros2_service'", result.stderr)
        self.assertEqual(generated, set())

    def test_module_without_generator_raises(self):
        project = {
            "project": {"_namespace_cpp": "demo"},
            "modules": [{"name": "Server", "type": "rpc_server", "transport": "ros2",
                         "ros2_package": "example_ros2", "_has_ros2_service_info": False}],
        }
        with self.assertRaisesRegex(ModuleGenError, r"no generator for module 'Server' "
                                    r"\(type=rpc_server, transport=ros2\)"):
            generate_modules(project)


if __name__ == "__main__":
    unittest.main()
