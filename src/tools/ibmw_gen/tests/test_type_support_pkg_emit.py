#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

"""Contract tests for generated TypeSupport packages."""

import pathlib
import filecmp
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

from generators.cmake_gen import generate_cmake
from generators.type_support_pkg_gen import (
    collect_package_type_support,
    generate_type_support_package_source,
    generate_type_support_package_sources,
)
from schema import load_and_validate


class TestTypeSupportPkgEmit(unittest.TestCase):
    def _load_sample(self, relative_path: str) -> dict:
        return load_and_validate(str(REPO_ROOT / relative_path))

    def test_idl_dds_package_exports_type_and_schema_support(self):
        project = self._load_sample(
            "src/samples/cpp/transport_demo/transport_demo.ibmw.yaml")
        spec = collect_package_type_support(
            project, project["apps"][0]["modules"])
        source = generate_type_support_package_source(spec)

        self.assertEqual([entry.family for entry in spec.entries], ["dds"])
        self.assertIn('#include "type_support/dds/dds_type_support.h"', source)
        self.assertIn('#include "ExampleEventSchema.hpp"', source)
        self.assertIn(
            "GetDdsMessageTypeSupportByName<"
            "ibmw::protocols::example::ExampleEventMsg>", source)
        self.assertIn(
            '>("ibmw::protocols::example::ExampleEventMsg", '
            'ibmw::dds::schema::kExampleEventIdl, '
            '"pubsub_demo::ExampleEventMsg")', source)
        self.assertIn('"pubsub_demo::ExampleEventMsg"', source)
        self._assert_framework_owned_typesupport(source)
        self._assert_schema_v1_exports(source)

        cmake = generate_cmake(project)
        self.assertIn("SRCS src/pubsub_pkg/pkg_main.cc", cmake)
        self.assertNotIn(
            "SRCS src/pubsub_pkg/pkg_main.cc "
            "src/pubsub_pkg/type_support_pkg_main.cc", cmake)
        self.assertIn(
            "add_library(pubsub_pkg_ts SHARED\n"
            "  src/pubsub_pkg/type_support_pkg_main.cc)", cmake)
        self.assertIn("ibmw::api::dds_type_support", cmake)
        self.assertIn("install(TARGETS pubsub_pkg_ts", cmake)
        self._assert_framework_owned_ts_link(cmake)
        self.assertIn("ibmw::schema::example_idl_gencode", cmake)

    def test_custom_dds_package_embeds_omg_idl_schema(self):
        project = self._load_sample(
            "src/samples/cpp/dds_image_demo/dds_image_demo.ibmw.yaml")
        spec = collect_package_type_support(
            project, project["apps"][0]["modules"])
        source = generate_type_support_package_source(spec)

        self.assertEqual(len(spec.entries), 1)
        self.assertTrue(spec.entries[0].custom_dds)
        self.assertIn('#include "protocol/image_msg.h"', source)
        self.assertIn("const unsigned long long kImageBufferSize = 1048576;",
                      source)
        self.assertIn("enum ImageEncoding", source)
        self.assertIn("@value(3) GRAY16", source)
        self.assertIn("octet data[kImageBufferSize];", source)
        self.assertIn("kIbmwCustomDdsSchema", source)
        self.assertIn("GetDdsMessageSchemaSupport<", source)
        self._assert_framework_owned_typesupport(source)

        cmake = generate_cmake(project)
        self.assertIn("ibmw::api::dds_type_support", cmake)
        self._assert_framework_owned_ts_link(cmake)
        self.assertIn("fastcdr", cmake)

    def test_ros2_package_links_introspection_and_exports_schema(self):
        project = self._load_sample(
            "src/samples/cpp/ros2_chn/ros2_chn.ibmw.yaml")
        spec = collect_package_type_support(
            project, project["apps"][0]["modules"])
        source = generate_type_support_package_source(spec)

        self.assertEqual(len(spec.entries), 1)
        self.assertEqual(spec.entries[0].family, "ros2")
        self.assertIn(
            '#include "type_support/ros2/ros2_schema_support.h"', source)
        self.assertIn(
            '#include "type_support/ros2/ros2_type_support_factory.h"', source)
        self.assertIn('#include "example_ros2/msg/ros_test_msg.hpp"', source)
        self.assertIn(
            "GetRos2MessageTypeSupport<example_ros2::msg::RosTestMsg>",
            source)
        self.assertIn(
            "GetRos2MessageSchemaSupport<example_ros2::msg::RosTestMsg>",
            source)
        self._assert_framework_owned_typesupport(source)
        self._assert_schema_v1_exports(source)

        cmake = generate_cmake(project)
        self.assertIn("ibmw::api::ros2_type_support", cmake)
        self._assert_framework_owned_ts_link(cmake)
        self.assertIn(
            "example_ros2::example_ros2__rosidl_typesupport_introspection_cpp",
            cmake)

    def test_mixed_package_has_one_deduplicated_entrypoint(self):
        modules = [
            {
                "name": "DdsPublisher",
                "type": "publisher",
                "transport": "dds",
                "message_type": "DdsMessage",
            },
            {
                "name": "DdsSubscriber",
                "type": "subscriber",
                "transport": "dds",
                "message_type": "DdsMessage",
            },
            {
                "name": "RosPublisher",
                "type": "publisher",
                "transport": "ros2",
                "message_type": "demo_msgs::msg::State",
                "ros2_msg_include": "demo_msgs/msg/state.hpp",
                "ros2_package": "demo_msgs",
            },
            {
                "name": "RosSubscriber",
                "type": "subscriber",
                "transport": "ros2",
                "message_type": "demo_msgs::msg::State",
                "ros2_msg_include": "demo_msgs/msg/state.hpp",
                "ros2_package": "demo_msgs",
            },
            {
                "name": "JsonPublisher",
                "type": "publisher",
                "transport": "json",
                "message_type": "Status",
            },
        ]
        project = {
            "project": {
                "name": "mixed_demo",
                "_namespace_cpp": "mixed_demo",
            },
            "_module_map": {module["name"]: module for module in modules},
            "modules": modules,
            "apps": [{
                "name": "mixed_app",
                "_snake_name": "mixed_app",
                "_pkg_name": "mixed_pkg",
                "modules": [module["name"] for module in modules],
            }],
            "dds_types": {
                "_namespace_cpp": "wire::demo",
                "_msg_to_group": {"DdsMessage": "Demo"},
                "groups": [{
                    "name": "Demo",
                    "idl_gencode_target": "demo_idl",
                    "messages": [{"name": "DdsMessage", "fields": []}],
                }],
            },
            "json_types": {
                "messages": [{"name": "Status", "fields": []}],
            },
        }

        generated = generate_type_support_package_sources(project)
        self.assertEqual(len(generated), 1)
        self.assertEqual(
            generated[0][0], "src/mixed_pkg/type_support_pkg_main.cc")
        source = generated[0][1]
        self.assertEqual(source.count("GetDdsMessageTypeSupportByName<"), 1)
        self.assertEqual(source.count("GetRos2MessageTypeSupport<"), 1)
        self.assertEqual(source.count("GetJsonMessageTypeSupport<"), 1)
        self._assert_framework_owned_typesupport(source)
        self._assert_schema_v1_exports(source)

        cmake = generate_cmake(project)
        self.assertIn("SRCS src/mixed_pkg/pkg_main.cc", cmake)
        self.assertIn(
            "add_library(mixed_pkg_ts SHARED\n"
            "  src/mixed_pkg/type_support_pkg_main.cc)", cmake)
        self._assert_framework_owned_ts_link(cmake)
        self.assertNotIn(
            "SRCS src/mixed_pkg/pkg_main.cc src/json_type_support_pkg_main.cc",
            cmake)

    def _assert_framework_owned_typesupport(self, generated: str) -> None:
        """Generated TypeSupport sidecars must not consume extension internals."""
        for forbidden in (
            '"dds_types/',
            '"ros2_types/',
            "ibmw::extensions::dds_types",
            "ibmw::extensions::ros2_types",
        ):
            self.assertNotIn(forbidden, generated)

    def _assert_framework_owned_ts_link(self, cmake: str) -> None:
        """Only the generated ``*_ts`` sidecar is covered by this boundary."""
        marker = "target_link_libraries("
        offset = 0
        while True:
            start = cmake.find(marker, offset)
            if start < 0:
                return
            end = cmake.find(")", start)
            self.assertGreater(end, start)
            block = cmake[start:end]
            if "_ts PRIVATE" in block:
                self._assert_framework_owned_typesupport(block)
            offset = end + 1

    def _assert_schema_v1_exports(self, source: str) -> None:
        for symbol in (
            "IbmwDynlibGetTypeSupportPackageV1",
        ):
            self.assertEqual(source.count(f"{symbol}()"), 1)
        self.assertIn(
            ".abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1", source)
        self.assertIn(
            ".struct_size = sizeof(ibmw_schema_support_package_v1_t)", source)
        self.assertNotIn("IbmwDynlibGetSchemaSupportArrayLength", source)
        self.assertNotIn("IbmwDynlibGetSchemaSupportArray()", source)

    def test_cli_generation_is_complete_idempotent_and_buildable(self):
        """Exercise the real CLI in an isolated tree, including configure/link."""
        fixture = REPO_ROOT / "src/samples/cpp/transport_demo/transport_demo_gen"
        yaml_file = REPO_ROOT / "src/samples/cpp/transport_demo/transport_demo.ibmw.yaml"
        generator = ROOT / "ibmw_gen.py"
        with tempfile.TemporaryDirectory(prefix="ibmw-gen-test-") as temp:
            temp_root = pathlib.Path(temp)
            output = temp_root / "generated"
            subprocess.run(
                [sys.executable, str(generator), str(yaml_file), "-o", str(output)],
                check=True,
                cwd=ROOT,
            )

            expected = {
                path.relative_to(fixture)
                for path in fixture.rglob("*")
                if path.is_file() and path.name != ".gitkeep"
            }
            actual = {
                path.relative_to(output)
                for path in output.rglob("*")
                if path.is_file()
            }
            self.assertEqual(actual, expected)
            for relative in sorted(expected):
                self.assertTrue(
                    filecmp.cmp(output / relative, fixture / relative, shallow=False),
                    relative,
                )

            before = {
                relative: (output / relative).read_bytes() for relative in expected
            }
            subprocess.run(
                [sys.executable, str(generator), str(yaml_file), "-o", str(output)],
                check=True,
                cwd=ROOT,
            )
            self.assertEqual(
                before,
                {relative: (output / relative).read_bytes() for relative in expected},
            )

            build_dir = pathlib.Path(
                os.environ.get("IBMW_BUILD_DIR", REPO_ROOT / "build-smoke-runtime-graph")
            )
            if not (build_dir / "install" / "ibmw-config.cmake").is_file():
                self.fail(
                    "IBMW_BUILD_DIR must point to a configured build with an install/ibmw-config.cmake"
                )
            install_prefix = temp_root / "ibmw-install"
            subprocess.run(
                ["cmake", "--install", str(build_dir), "--prefix", str(install_prefix)],
                check=True,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            cmake_build = temp_root / "cmake-build"
            subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(output),
                    "-B",
                    str(cmake_build),
                    "-DCMAKE_PREFIX_PATH=" + str(install_prefix),
                    "-DCMAKE_EXE_LINKER_FLAGS=-lgcov",
                    "-DCMAKE_SHARED_LINKER_FLAGS=-lgcov",
                ],
                check=True,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            subprocess.run(
                [
                    "cmake", "--build", str(cmake_build), "--target",
                    "pubsub_pkg", "pubsub_pkg_ts", "-j2",
                ],
                check=True,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )


if __name__ == "__main__":
    unittest.main()
