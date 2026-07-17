#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]
IBMW_GEN = ROOT / "ibmw_gen.py"
COMMON_DIR = (
    ROOT.parent.parent / "samples" / "cpp" / "ros2_action_fibonacci_common"
)
SAMPLE_YAML = (
    ROOT.parent.parent
    / "samples"
    / "cpp"
    / "ros2_action_fibonacci_gen"
    / "ros2_action_fibonacci_gen.ibmw.yaml"
)
sys.path.insert(0, str(ROOT))


def _minimal_project(action_block: list[dict[str, object]]) -> dict[str, object]:
    return {
        "project": {"name": "action_exp", "namespace": "action_exp"},
        "modules": [],
        "apps": [],
        "experimental_actions": action_block,
    }


def _write_yaml(data: dict[str, object]) -> pathlib.Path:
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".ibmw.yaml", delete=False, encoding="utf-8"
    ) as f:
        _ = yaml.safe_dump(data, f)
        return pathlib.Path(f.name)


def _run_gen_validate_only(data: dict[str, object]) -> subprocess.CompletedProcess[str]:
    yaml_path = _write_yaml(data)
    out_dir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_action_validate_"))
    try:
        return subprocess.run(
            [sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out_dir), "--dry-run"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    finally:
        yaml_path.unlink(missing_ok=True)
        shutil.rmtree(out_dir, ignore_errors=True)


class TestExperimentalActions(unittest.TestCase):
    def test_schema_accepts_fibonacci_only(self) -> None:
        data = _minimal_project([{
            "name": "fibonacci",
            "action": "example_interfaces/action/Fibonacci",
            "transport": "dds",
            "roles": ["server", "client"],
        }])
        result = _run_gen_validate_only(data)
        self.assertEqual(result.returncode, 0,
                         f"stdout={result.stdout}\nstderr={result.stderr}")
        self.assertIn("actions", result.stdout)

    def test_schema_rejects_non_fibonacci_action(self) -> None:
        data = _minimal_project([{
            "name": "navigate",
            "action": "nav2_msgs/action/NavigateToPose",
            "transport": "dds",
        }])
        result = _run_gen_validate_only(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Fibonacci-only", result.stderr)

    def test_schema_rejects_non_dds_transport(self) -> None:
        data = _minimal_project([{
            "name": "fibonacci",
            "action": "example_interfaces/action/Fibonacci",
            "transport": "ros2",
            "roles": ["server"],
        }])
        result = _run_gen_validate_only(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("supports only transport: dds", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_schema_rejects_non_string_roles_without_traceback(self) -> None:
        data = _minimal_project([{
            "name": "fibonacci",
            "action": "example_interfaces/action/Fibonacci",
            "transport": "dds",
            "roles": [["server"]],
        }])
        result = _run_gen_validate_only(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("roles must be strings", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_schema_keeps_action_out_of_module_types(self) -> None:
        data = _minimal_project([])
        data["modules"] = [{"name": "BadAction", "type": "action_server"}]
        result = _run_gen_validate_only(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid type 'action_server'", result.stderr)

    def test_generator_emits_private_sources_and_no_rcl_cmake_boundary(self) -> None:
        tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_action_"))
        try:
            yaml_path = tmpdir / "action_exp.ibmw.yaml"
            out_dir = tmpdir / "out"
            _ = yaml_path.write_text(yaml.safe_dump(_minimal_project([{
                "name": "fibonacci",
                "action": "example_interfaces/action/Fibonacci",
                "transport": "dds",
                "roles": ["server", "client"],
            }])), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out_dir)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0,
                             f"stdout={result.stdout}\nstderr={result.stderr}")

            cmake_text = (out_dir / "CMakeLists.txt").read_text(encoding="utf-8")
            server_text = (out_dir / "src/experimental_actions/fibonacci/server/fibonacci_action_server.cpp").read_text(encoding="utf-8")
            client_text = (out_dir / "src/experimental_actions/fibonacci/client/fibonacci_action_client.cpp").read_text(encoding="utf-8")
            common_header = out_dir / "src/experimental_actions/fibonacci/common/fibonacci_action_common.h"
            traits_header = out_dir / "src/experimental_actions/fibonacci/common/fibonacci_action_traits.h"
            internal_header_dir = out_dir / "src/experimental_actions/fibonacci/common/internal/ros2_action_dds"
            public_action_dir = out_dir / "src/experimental_actions/fibonacci/common/cpp/action"

            self.assertTrue(common_header.exists())
            self.assertTrue(traits_header.exists(),
                            "missing copied public Action traits header")
            for header in [
                "action_wire_contract.h",
                "fastdds_wire_contract.h",
                "fastdds_typesupport.h",
                "action_goal_ledger.h",
                "action_client_observer.h",
            ]:
                self.assertTrue((internal_header_dir / header).exists(),
                                f"missing copied internal Action DDS header: {header}")
            for header in [
                "action_traits.h",
                "server.h",
                "client.h",
            ]:
                self.assertTrue((public_action_dir / header).exists(),
                                f"missing copied public Action facade header: {header}")
            self.assertIn("IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY", cmake_text)
            self.assertIn("IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY", server_text)
            self.assertIn("IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY", client_text)
            self.assertIn("ibmw_gen_fibonacci_action_dds_server", cmake_text)
            self.assertIn("ibmw_gen_fibonacci_action_dds_client", cmake_text)
            self.assertTrue(
                server_text.startswith(
                    "// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame."
                )
            )
            self.assertEqual(
                server_text.splitlines()[3],
                "// IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY",
            )
            self.assertNotIn("file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/cfg", cmake_text)
            self.assertIn("../common/fibonacci_action_common.h", server_text)
            self.assertIn("../common/fibonacci_action_common.h", client_text)
            self.assertIn("../common/fibonacci_action_traits.h", server_text)
            self.assertIn("../common/fibonacci_action_traits.h", client_text)
            self.assertIn('#include "cpp/action/server.h"', server_text)
            self.assertIn('#include "cpp/action/client.h"', client_text)
            forbidden = ["rclcpp", "rclcpp_action", "rcl_action", "find_package(rmw"]
            for token in forbidden:
                self.assertNotIn(token, cmake_text)
            self.assertNotIn("ibmw::internal::ros2_action_dds", cmake_text)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def test_generator_honors_action_role_subset(self) -> None:
        tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_action_subset_"))
        try:
            yaml_path = tmpdir / "action_exp.ibmw.yaml"
            out_dir = tmpdir / "out"
            _ = yaml_path.write_text(yaml.safe_dump(_minimal_project([{
                "name": "fibonacci",
                "action": "example_interfaces/action/Fibonacci",
                "transport": "dds",
                "roles": ["server"],
            }])), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out_dir)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0,
                             f"stdout={result.stdout}\nstderr={result.stderr}")
            cmake_text = (out_dir / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertTrue(
                (out_dir / "src/experimental_actions/fibonacci/server/fibonacci_action_server.cpp").exists()
            )
            self.assertFalse(
                (out_dir / "src/experimental_actions/fibonacci/client/fibonacci_action_client.cpp").exists()
            )
            self.assertIn("ibmw_gen_fibonacci_action_dds_server", cmake_text)
            self.assertNotIn("ibmw_gen_fibonacci_action_dds_client", cmake_text)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def test_in_tree_sample_yaml_generates_action_proof(self) -> None:
        tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_action_sample_"))
        try:
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), str(SAMPLE_YAML), "-o", str(tmpdir)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0,
                             f"stdout={result.stdout}\nstderr={result.stderr}")
            self.assertTrue(
                (tmpdir / "src/experimental_actions/fibonacci/server/fibonacci_action_server.cpp").exists()
            )
            self.assertTrue(
                (tmpdir / "src/experimental_actions/fibonacci/client/fibonacci_action_client.cpp").exists()
            )
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


    def test_emitter_byte_equals_handwritten_typesupport(self) -> None:
        from generators.action_gen import (
            _derive_action_meta,
            _render_typesupport_header,
        )

        meta = _derive_action_meta("example_interfaces/action/Fibonacci")
        emitted = _render_typesupport_header(meta)
        handwritten = (
            COMMON_DIR / "fibonacci_action_fastdds_typesupport.h"
        ).read_text(encoding="utf-8")
        self.assertEqual(emitted, handwritten)

    def test_emitted_typesupport_locks_dds_type_names(self) -> None:
        expected = [
            "example_interfaces::action::dds_::Fibonacci_SendGoal_Request_",
            "example_interfaces::action::dds_::Fibonacci_SendGoal_Response_",
            "example_interfaces::action::dds_::Fibonacci_GetResult_Request_",
            "example_interfaces::action::dds_::Fibonacci_GetResult_Response_",
            "example_interfaces::action::dds_::Fibonacci_FeedbackMessage_",
        ]
        tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_action_dds_"))
        try:
            yaml_path = tmpdir / "action_exp.ibmw.yaml"
            out_dir = tmpdir / "out"
            _ = yaml_path.write_text(yaml.safe_dump(_minimal_project([{
                "name": "fibonacci",
                "action": "example_interfaces/action/Fibonacci",
                "transport": "dds",
                "roles": ["server", "client"],
            }])), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out_dir)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0,
                             f"stdout={result.stdout}\nstderr={result.stderr}")
            text = (
                out_dir / "src/experimental_actions/fibonacci/common/"
                "fibonacci_action_fastdds_typesupport.h"
            ).read_text(encoding="utf-8")
            found = re.findall(r'kName\s*=\s*"([^"]+)"', text)
            self.assertEqual(found, expected)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


class TestActionMetadataEngineGeneric(unittest.TestCase):
    """Lock the action metadata engine as generic against a second action.

    The public ``experimental_actions`` surface is Fibonacci-only by policy
    (see ``TestExperimentalActions.test_schema_rejects_non_fibonacci_action``).
    These tests bypass that lane and call the derivation/render functions
    directly so a future regression to Fibonacci-hardcoded metadata is caught.
    """

    SECOND_ACTION = "nav2_msgs/action/NavigateToPose"

    def test_derive_action_meta_is_generic(self) -> None:
        from generators.action_gen import _derive_action_meta

        meta = _derive_action_meta(self.SECOND_ACTION)
        self.assertEqual(meta["pkg"], "nav2_msgs")
        self.assertEqual(meta["name"], "NavigateToPose")
        self.assertEqual(meta["snake"], "navigate_to_pose")
        self.assertEqual(meta["cpp_root"], "nav2_msgs::action::NavigateToPose")
        self.assertEqual(
            meta["ts_ns"], "nav2_msgs::action::typesupport_fastrtps_cpp"
        )
        self.assertEqual(
            meta["sample_ns"], "ibmw::samples::ros2_action_navigate_to_pose"
        )

    def test_derive_action_meta_rejects_malformed_spec(self) -> None:
        from generators.action_gen import _derive_action_meta

        for bad in [
            "nav2_msgs/NavigateToPose",
            "nav2_msgs/srv/NavigateToPose",
            "example_interfaces/action",
            "a/action/B/C",
        ]:
            with self.assertRaises(ValueError):
                _derive_action_meta(bad)

    def test_render_typesupport_header_is_generic(self) -> None:
        from generators.action_gen import (
            _derive_action_meta,
            _render_typesupport_header,
        )

        meta = _derive_action_meta(self.SECOND_ACTION)
        header = _render_typesupport_header(meta)

        guard = (
            "IBMW_SAMPLES_CPP_ROS2_ACTION_NAVIGATE_TO_POSE_COMMON_"
            "NAVIGATE_TO_POSE_ACTION_FASTDDS_TYPESUPPORT_H_"
        )
        self.assertIn(f"#ifndef {guard}", header)
        self.assertIn(f"#define {guard}", header)
        self.assertIn(f"#endif  // {guard}", header)

        self.assertIn(
            "#include <nav2_msgs/action/detail/"
            "navigate_to_pose__rosidl_typesupport_fastrtps_cpp.hpp>",
            header,
        )
        self.assertIn(
            "#include <nav2_msgs/action/navigate_to_pose.hpp>", header
        )

        self.assertIn(
            "namespace ibmw::samples::ros2_action_navigate_to_pose {", header
        )
        self.assertIn(
            "using NavigateToPose = nav2_msgs::action::NavigateToPose;", header
        )

    def test_render_typesupport_header_locks_second_action_dds_type_names(self) -> None:
        from generators.action_gen import (
            _derive_action_meta,
            _render_typesupport_header,
        )

        meta = _derive_action_meta(self.SECOND_ACTION)
        header = _render_typesupport_header(meta)
        found = re.findall(r'kName\s*=\s*"([^"]+)"', header)
        self.assertEqual(found, [
            "nav2_msgs::action::dds_::NavigateToPose_SendGoal_Request_",
            "nav2_msgs::action::dds_::NavigateToPose_SendGoal_Response_",
            "nav2_msgs::action::dds_::NavigateToPose_GetResult_Request_",
            "nav2_msgs::action::dds_::NavigateToPose_GetResult_Response_",
            "nav2_msgs::action::dds_::NavigateToPose_FeedbackMessage_",
        ])


if __name__ == "__main__":
    _ = unittest.main()
