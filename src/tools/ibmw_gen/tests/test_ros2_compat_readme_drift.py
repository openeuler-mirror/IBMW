#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

import pathlib
import unittest
from typing import cast

import yaml


SAMPLES_DIR = pathlib.Path(__file__).resolve().parents[3] / "samples" / "cpp"
IBMW_GEN_README = pathlib.Path(__file__).resolve().parents[1] / "README.md"
DDS_RPC_DIR = SAMPLES_DIR / "dds_rpc_demo"
ROS2_INTEROP_DIR = SAMPLES_DIR / "ros2_interop_demo"
ACTION_GEN_DIR = SAMPLES_DIR / "ros2_action_fibonacci_gen"


def _read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def _yaml(path: pathlib.Path) -> dict[str, object]:
    loaded = cast(object, yaml.safe_load(_read(path)))
    return _mapping(loaded, str(path))


def _mapping(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise AssertionError(f"expected mapping for {label}")
    raw = cast(dict[object, object], value)
    return {str(k): v for k, v in raw.items()}


def _sequence(value: object, label: str) -> list[object]:
    if not isinstance(value, list):
        raise AssertionError(f"expected sequence for {label}")
    return cast(list[object], value)


class TestRos2CompatReadmeDrift(unittest.TestCase):
    def test_ibmw_gen_readme_has_current_msg_srv_action_matrix(self) -> None:
        readme = _read(IBMW_GEN_README)
        ros2_yaml = _yaml(ROS2_INTEROP_DIR / "ros2_interop_demo.ibmw.yaml")
        dds_rpc_yaml = _yaml(DDS_RPC_DIR / "dds_rpc_demo.ibmw.yaml")
        action_yaml = _yaml(ACTION_GEN_DIR / "ros2_action_fibonacci_gen.ibmw.yaml")

        ros2_modules = _sequence(ros2_yaml["modules"], "ros2 modules")
        ros2_module_types = [
            str(_mapping(module, f"ros2 modules[{index}]")["type"])
            for index, module in enumerate(ros2_modules)
        ]
        dds_types = _mapping(dds_rpc_yaml["dds_types"], "dds_types")
        rpc_services = _sequence(dds_types["rpc_services"], "rpc_services")
        rpc_service = _mapping(rpc_services[0], "rpc_services[0]")
        actions = _sequence(action_yaml["experimental_actions"], "experimental_actions")
        action = _mapping(actions[0], "experimental_actions[0]")

        required_tokens = [
            "ROS2-Compatible DDS-Only Usability Matrix",
            "MSG pub/sub",
            "SRV request/response",
            "Action goal/feedback/result",
            "messages.<cpp_type>.wire.ros2",
            "direct_ros2_publisher",
            "direct_ros2_subscriber",
            "dds_types.rpc_services[].srv",
            str(rpc_service["srv"]),
            "experimental_actions",
            str(action["action"]),
            "Fibonacci-only",
            "no public `action_server` / `action_client` module types",
        ]
        for token in required_tokens:
            self.assertIn(token, readme)

        for module_type in ros2_module_types:
            self.assertIn(module_type, readme)

        forbidden_claims = [
            "Generic `.action` parsing is supported",
            "public Action runtime is supported",
            "action_server` | Action service server",
            "action_client` | Action service client",
        ]
        for claim in forbidden_claims:
            self.assertNotIn(claim, readme)

    def test_dds_rpc_readme_matches_add_two_ints_srv_sample(self) -> None:
        readme = _read(DDS_RPC_DIR / "README.md")
        source_yaml = _yaml(DDS_RPC_DIR / "dds_rpc_demo.ibmw.yaml")
        service_yaml = _yaml(
            DDS_RPC_DIR
            / "dds_rpc_demo_gen"
            / "schema"
            / "AddTwoIntsService.yaml"
        )

        dds_types = _mapping(source_yaml["dds_types"], "dds_types")
        rpc_services = _sequence(dds_types["rpc_services"], "rpc_services")
        service = _mapping(rpc_services[0], "rpc_services[0]")
        methods = _sequence(service["methods"], "rpc_services[0].methods")
        method = _mapping(methods[0], "rpc_services[0].methods[0]")

        generated_services = _sequence(service_yaml["services"], "services")
        generated_service = _mapping(generated_services[0], "services[0]")
        generated_methods = _sequence(
            generated_service["methods"], "services[0].methods"
        )
        generated_method = _mapping(generated_methods[0], "services[0].methods[0]")

        required_tokens = [
            str(service["srv"]),
            str(service["name"]),
            str(method["name"]),
            str(method["request"]),
            str(method["response"]),
            str(generated_service["ros2_service_name"]),
            str(generated_method["req_dds_type_name"]),
            str(generated_method["rsp_dds_type_name"]),
            "schema/AddTwoInts.idl",
            "schema/AddTwoIntsService.yaml",
            "sync RPC server",
            "periodic sync RPC client",
        ]
        for token in required_tokens:
            self.assertIn(token, readme)

        stale_tokens = [
            "Calculator.idl",
            "CalculatorService/Add",
            "coroutine-based RPC server",
            "coroutine-based RPC client",
            "Add requests",
        ]
        for token in stale_tokens:
            self.assertNotIn(token, readme)

    def test_ros2_interop_readme_matches_direct_ros2_sample(self) -> None:
        readme = _read(ROS2_INTEROP_DIR / "README.md")
        source_yaml = _yaml(ROS2_INTEROP_DIR / "ros2_interop_demo.ibmw.yaml")
        app_cfg = _yaml(
            ROS2_INTEROP_DIR
            / "ros2_interop_demo_gen"
            / "cfg"
            / "ros2_interop_app.yaml"
        )
        cmake_text = _read(
            ROS2_INTEROP_DIR / "ros2_interop_demo_gen" / "CMakeLists.txt"
        )

        modules = _sequence(source_yaml["modules"], "modules")
        module_by_name: dict[str, dict[str, object]] = {}
        for index, module in enumerate(modules):
            module_map = _mapping(module, f"modules[{index}]")
            module_by_name[str(module_map["name"])] = module_map
        dds_driver = _mapping(source_yaml["dds_driver"], "dds_driver")
        messages = _mapping(source_yaml["messages"], "messages")
        _ = _mapping(
            messages["std_msgs::msg::String"], "messages.std_msgs::msg::String"
        )

        ibmw_cfg = _mapping(app_cfg["ibmw"], "ibmw")
        extensions = _mapping(ibmw_cfg["extensions"], "ibmw.extensions")
        items = _sequence(extensions["items"], "ibmw.extensions.items")
        dds_extension = _mapping(items[0], "ibmw.extensions.items[0]")
        extension_options = _mapping(
            dds_extension["options"], "ibmw.extensions.items[0].options"
        )

        required_tokens = [
            str(module_by_name["PublisherModule"]["type"]),
            str(module_by_name["SubscriberModule"]["type"]),
            str(module_by_name["PublisherModule"]["topic"]),
            str(module_by_name["PublisherModule"]["message"]),
            str(dds_driver["ros2_topic_prefix"]),
            "std_msgs/msg/string.hpp",
            "std_msgs/msg/String",
            "RegisterSendType",
            "Send",
            "OnMessage",
            "ros2_interop_app",
            "ros2_pub_only_app",
            "ros2_sub_only_app",
            "publisher_module_pkg.yaml",
            "subscriber_module_pkg.yaml",
        ]
        for token in required_tokens:
            self.assertIn(token, readme)

        self.assertIn("domain_id", extension_options)
        self.assertNotIn("ros2_compatible_mode", extension_options)
        self.assertNotIn("ros2_topic_prefix", extension_options)
        self.assertIn("ros2_compatible_mode: true", readme)
        self.assertIn("ros2_topic_prefix: \"rt\"", readme)

        messaging = _mapping(ibmw_cfg["messaging"], "ibmw.messaging")
        drivers = _sequence(messaging["drivers"], "ibmw.messaging.drivers")
        dds_driver_cfg = _mapping(drivers[0], "ibmw.messaging.drivers[0]")
        dds_options = _mapping(
            dds_driver_cfg["options"], "ibmw.messaging.drivers[0].options"
        )
        self.assertEqual(dds_driver_cfg["type"], "dds")
        self.assertEqual(dds_options["ros2_compatible"], True)
        self.assertEqual(dds_options["ros2_topic_prefix"], "rt")
        publishers = _sequence(messaging["publishers"], "ibmw.messaging.publishers")
        subscribers = _sequence(messaging["subscribers"], "ibmw.messaging.subscribers")
        publisher = _mapping(publishers[0], "ibmw.messaging.publishers[0]")
        subscriber = _mapping(subscribers[0], "ibmw.messaging.subscribers[0]")
        self.assertEqual(publisher["topic"], "chatter")
        self.assertEqual(publisher["enable_drivers"], ["dds"])
        self.assertEqual(subscriber["topic"], "chatter")
        self.assertEqual(subscriber["enable_drivers"], ["dds"])

        for target in [
            "ros2_interop_app",
            "ros2_pub_only_app",
            "ros2_sub_only_app",
        ]:
            self.assertIn(target, cmake_text)
            self.assertIn(target, readme)

        stale_tokens = [
            "type: ros2_interop_publisher",
            "message_type:",
            "ros2_msg_include:",
            "ros2_type_name:",
            "ros2_interop_cfg.yaml",
            "test_with_ros2_talker.sh",
            "test_with_ros2_listener.sh",
            "src/interface/ibmw_module_ros2_interface",
        ]
        for token in stale_tokens:
            self.assertNotIn(token, readme)


if __name__ == "__main__":
    _ = unittest.main()
