#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Focused CMake generator test for cross-compile RPATH handling.
#
# Usage:
#   python3 -m pytest tests/test_cmake_cross_rpath.py -v

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))

from generators.cmake_gen import generate_cmake


class TestCMakeCrossRpath(unittest.TestCase):
    def test_cross_compiling_disables_rpath(self):
        cmake = generate_cmake({
            "project": {
                "name": "demo_project",
                "_namespace_cpp": "ibmw::demo",
            },
            "_module_map": {},
            "modules": [],
            "apps": [],
        })

        self.assertIn("if(CMAKE_CROSSCOMPILING)", cmake)
        self.assertIn("set(CMAKE_SKIP_RPATH ON)", cmake)
        self.assertIn("set(CMAKE_SKIP_BUILD_RPATH ON)", cmake)
        self.assertIn("set(CMAKE_SKIP_INSTALL_RPATH ON)", cmake)
        self.assertIn("set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)", cmake)

    def test_imported_ros2_srv_adapter_links_runtime_glue(self):
        cmake = generate_cmake({
            "project": {
                "name": "demo_project",
                "_namespace_cpp": "ibmw::demo",
            },
            "_module_map": {
                "BatchEchoRpcClientModule": {
                    "name": "BatchEchoRpcClientModule",
                    "type": "rpc_client",
                    "transport": "ros2",
                    "ros2_package": "robot_interfaces",
                    "ros2_service": "BatchEcho",
                    "ros2_imported_srv_adapter": True,
                }
            },
            "modules": [
                {
                    "name": "BatchEchoRpcClientModule",
                    "type": "rpc_client",
                    "transport": "ros2",
                    "ros2_package": "robot_interfaces",
                    "ros2_service": "BatchEcho",
                    "ros2_imported_srv_adapter": True,
                }
            ],
            "apps": [],
        })

        self.assertIn("find_package(robot_interfaces REQUIRED)", cmake)
        self.assertIn("find_package(rosidl_typesupport_cpp REQUIRED)", cmake)
        self.assertIn("ibmw_add_rosidl_interface_target(robot_interfaces_ibmw_imported_srv_glue", cmake)
        self.assertIn("DEPS robot_interfaces::robot_interfaces__rosidl_typesupport_cpp", cmake)
        self.assertIn("rosidl_typesupport_cpp::rosidl_typesupport_cpp)", cmake)
        self.assertIn("robot_interfaces_ibmw_imported_srv_glue", cmake)

    def test_imported_ros2_srv_adapter_adds_reference_include_dir(self):
        cmake = generate_cmake({
            "project": {
                "name": "demo_project",
                "_namespace_cpp": "ibmw::demo",
            },
            "_module_map": {
                "SetFlagRpcServerModule": {
                    "name": "SetFlagRpcServerModule",
                    "type": "rpc_server",
                    "transport": "ros2",
                    "ros2_package": "robot_interfaces",
                    "ros2_service": "SetFlag",
                    "ros2_imported_srv_adapter": True,
                    "ros2_imported_srv_adapter_include_dir": "/tmp/reference/include",
                }
            },
            "modules": [
                {
                    "name": "SetFlagRpcServerModule",
                    "type": "rpc_server",
                    "transport": "ros2",
                    "ros2_package": "robot_interfaces",
                    "ros2_service": "SetFlag",
                    "ros2_imported_srv_adapter": True,
                    "ros2_imported_srv_adapter_include_dir": "/tmp/reference/include",
                }
            ],
            "apps": [],
        })

        self.assertIn("target_include_directories(set_flag_rpc_server_module PUBLIC /tmp/reference/include)", cmake)

    def test_imported_json_types_header_adds_reference_include_dir(self):
        cmake = generate_cmake({
            "project": {
                "name": "demo_project",
                "_namespace_cpp": "ibmw::demo",
            },
            "_module_map": {},
            "modules": [
                {
                    "name": "PlainJsonPublisherModule",
                    "type": "publisher",
                    "transport": "json",
                    "message_type": "ibmw::ros2_interfaces::json::demo::Plain",
                    "topic": "/plain",
                    "ros2_import_json_types_header": "demo_json_types.hpp",
                    "ros2_import_json_types_include_dir": "/tmp/imported-json",
                }
            ],
            "apps": [],
        })

        self.assertIn("target_include_directories(plain_json_publisher_module PUBLIC /tmp/imported-json)", cmake)

    def test_default_json_types_header_adds_generated_src_include_dir(self):
        cmake = generate_cmake({
            "project": {
                "name": "demo_project",
                "_namespace_cpp": "ibmw::demo",
            },
            "_module_map": {},
            "modules": [
                {
                    "name": "PlainJsonPublisherModule",
                    "type": "publisher",
                    "transport": "json",
                    "message_type": "ibmw::demo::Plain",
                    "topic": "/plain",
                }
            ],
            "apps": [],
        })

        self.assertIn(
            "target_include_directories(plain_json_publisher_module PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)",
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
