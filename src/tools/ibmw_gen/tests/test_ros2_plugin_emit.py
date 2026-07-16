#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# ROS 2 plugin emit_module contract tests.
#
# Mirrors test_iceoryx_plugin_emit.py. Verifies:
#   1. ros2 plugin registered via transports.get("ros2").
#   2. emit_module callable exists rather than a None stub.
#   3. Plugin `owns_type_keys` covers publisher/subscriber/rpc_server/rpc_client.
#   4. module_gen._GENERATORS no longer contains any (*, "ros2") row.
#   5. Plugin-emitted bytes for synthetic publisher/subscriber/rpc_server dicts
#      are shaped like real generated code.
#   6. emit_module returns None for unknown type (fall-through contract).
#   7. rpc_server skeleton fallback (when _has_ros2_service_info missing) emits
#      a compilable placeholder, not the full typed stub.
#
# Usage:
#   python3 -m pytest tests/test_ros2_plugin_emit.py -v

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))


def _canonical_rpc_server_markers():
    header_path = REPO_ROOT / "src/api/cpp/rpc_marker.h"
    header_text = header_path.read_text(encoding="utf-8")
    return re.findall(
        r'inline constexpr char kRpcServerHandlerBody\[] = "([^"]+)";',
        header_text,
    )


def _canonical_rpc_client_markers():
    header_path = REPO_ROOT / "src/api/cpp/rpc_marker.h"
    header_text = header_path.read_text(encoding="utf-8")
    return re.findall(
        r'inline constexpr char kRpcClient(?:RequestParameterBuild|ResponseParameterConsume|ReturnStatusHandling)\[] =\s*"([^"]+)";',
        header_text,
    )


def _method_marker(marker: str, method_name: str) -> str:
    snake = re.sub(r"(?<!^)(?=[A-Z])", "_", method_name).upper()
    return f"{marker}_{snake}"


EXPECTED_ROS2_TYPE_KEYS = {
    "publisher",
    "subscriber",
    "rpc_server",
    "rpc_client",
}


class TestRos2PluginEmit(unittest.TestCase):
    def setUp(self):
        import transports
        from generators import module_gen

        self.transports = transports
        self.module_gen = module_gen
        self.spec = transports.get("ros2")

    def test_ros2_plugin_registered(self):
        self.assertIsNotNone(self.spec, "ros2 plugin not registered")
        self.assertEqual(self.spec.name, "ros2")

    def test_emit_module_callable(self):
        self.assertIsNotNone(
            self.spec.emit_module,
            "ros2 plugin emit_module is not wired",
        )

    def test_owns_all_expected_ros2_type_keys(self):
        owned = set(self.spec.meta.get("owns_type_keys", []))
        missing = EXPECTED_ROS2_TYPE_KEYS - owned
        self.assertFalse(
            missing,
            f"plugin does not claim these ros2 types: {sorted(missing)}",
        )

    def test_module_gen_no_longer_has_ros2_keys(self):
        # The legacy table must not contain any (*, "ros2") entries —
        # those generators all live in transports/ros2.py now.
        leftover = {
            (t, tr) for (t, tr) in self.module_gen._GENERATORS
            if tr == "ros2"
        }
        self.assertFalse(
            leftover,
            f"module_gen._GENERATORS still has ros2 keys after plugin extraction: "
            f"{sorted(leftover)} — these must move to transports/ros2.py",
        )

    def test_emit_publisher_shape_and_stability(self):
        # Publisher now uses the unified path.
        mod = {
            "name": "demo_pub",
            "type": "publisher",
            "transport": "ros2",
            "message_type": "example_ros2::msg::RosTestMsg",
            "ros2_msg_include": "example_ros2/msg/ros_test_msg.hpp",
            "topic": "test_topic",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_pub_module"

        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        ts_info = self.spec.emit_publisher_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"],
                         "example_ros2::msg::RosTestMsg")
        inc_h = ts_info["inc_h_content"]
        self.assertIn("internal/typesupport/ros2/ros2_transport.h", inc_h)
        self.assertIn("example_ros2/msg/ros_test_msg.hpp", inc_h)
        self.assertIn("IBMW_TS_SENDER_ACCESS_MEMBER", inc_h)
        # Stability.
        self.assertEqual(ts_info,
                         self.spec.emit_publisher_typesupport(mod, ns))

        # End-to-end unified path.
        project = {"project": {"_namespace_cpp": ns}, "modules": [dict(mod)]}
        from generators import module_gen
        all_out = module_gen.generate_modules(project)
        paths = {p for p, _ in all_out}
        self.assertEqual(
            paths,
            {
                f"src/{mod_dir}/demo_pub.h",
                f"src/{mod_dir}/demo_pub.cc",
                f"src/{mod_dir}/demo_pub_typesupport.inc.h",
                f"src/{mod_dir}/demo_pub_typesupport.inc.cc",
            },
        )
        out_by_path = dict(all_out)
        user_h = out_by_path[f"src/{mod_dir}/demo_pub.h"]
        self.assertNotIn("ros2_transport.h", user_h)
        self.assertNotIn("rclcpp/rclcpp.hpp", user_h)
        self.assertNotIn("SenderAccess<example_ros2::msg::RosTestMsg>",
                         user_h)
        self.assertIn("test_topic",
                      out_by_path[f"src/{mod_dir}/demo_pub_typesupport.inc.h"])

    def test_emit_subscriber_shape(self):
        # Subscriber now flows through the unified path.
        mod = {
            "name": "demo_sub",
            "type": "subscriber",
            "transport": "ros2",
            "message_type": "example_ros2::msg::RosTestMsg",
            "ros2_msg_include": "example_ros2/msg/ros_test_msg.hpp",
            "topic": "test_topic",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_sub_module"

        # emit_module defers subscriber to unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_subscriber_typesupport shape.
        self.assertIsNotNone(
            getattr(self.spec, "emit_subscriber_typesupport", None),
            "ros2 plugin emit_subscriber_typesupport hook missing — "
            "unified subscriber hook not wired",
        )
        ts_info = self.spec.emit_subscriber_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"],
                         "example_ros2::msg::RosTestMsg")
        self.assertIn("internal/typesupport/ros2/ros2_transport.h",
                      ts_info["inc_h_content"])
        self.assertIn("example_ros2/msg/ros_test_msg.hpp",
                      ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RECEIVER_MSG_TYPE",
                      ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RECEIVER_ACCESS_SETUP",
                      ts_info["inc_h_content"])

        # Unified end-to-end: 4 files + R1/R2 guards.
        project = {
            "project": {"_namespace_cpp": ns},
            "modules": [dict(mod)],
        }
        all_out = self.module_gen.generate_modules(project)
        paths = {p for p, _ in all_out}
        self.assertEqual(
            paths,
            {
                f"src/{mod_dir}/demo_sub.h",
                f"src/{mod_dir}/demo_sub.cc",
                f"src/{mod_dir}/demo_sub_typesupport.inc.h",
                f"src/{mod_dir}/demo_sub_typesupport.inc.cc",
            },
        )
        out_by_path = dict(all_out)
        user_h = out_by_path[f"src/{mod_dir}/demo_sub.h"]
        # R1: no ros2 transport/msg headers in user-facing .h.
        self.assertNotIn("ros2_types/transport/ros2_transport.h", user_h)
        self.assertNotIn("rclcpp/rclcpp.hpp", user_h)
        self.assertNotIn("example_ros2/msg/ros_test_msg.hpp", user_h)
        self.assertIn("demo_sub_typesupport.inc.h", user_h)
        # R2: no ReceiverAccess<ConcreteType> in main .cc.
        user_cc = out_by_path[f"src/{mod_dir}/demo_sub.cc"]
        self.assertNotIn("ReceiverAccess<example_ros2::msg::RosTestMsg>",
                         user_cc)
        self.assertIn("IBMW_TS_RECEIVER_ACCESS_SETUP", user_cc)
        self.assertIn("on_message_body", user_cc)
        self.assertIn("test_topic",
                      out_by_path[f"src/{mod_dir}/demo_sub_typesupport.inc.h"])

    def test_emit_rpc_server_skeleton_fallback(self):
        # plugin emit_module no longer handles rpc_server —
        # the unified path in module_gen._gen_rpc_server_unified handles all
        # rpc emission via _gen_ros2_rpc_server_markerized_typesupport
        # marker-preserving sidecar protocol. Plugin must defer (return None) for
        # rpc_server, mirroring the publisher/subscriber contract above.
        # Real-path shape coverage lives in
        # test_emit_ros2_rpc_server_real_typesupport_shape and
        # test_ros2_rpc_server_real_path_marker_contract below.
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "ros2",
            # Intentionally no ros2_package / ros2_service / _has_ros2_service_info.
        }
        self.assertIsNone(
            self.spec.emit_module(mod, "ibmw::demo", "demo_rpc_server_module")
        )

    def test_emit_ros2_rpc_server_real_typesupport_shape(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "ros2",
            "ros2_package": "example_ros2",
            "ros2_service": "RosTestRpc",
            "_has_ros2_service_info": True,
        }

        self.assertIsNotNone(
            getattr(self.spec, "emit_rpc_server_typesupport", None),
            "ros2 plugin emit_rpc_server_typesupport hook missing — "
            "unified RPC server hook not wired",
        )

        ts_info = self.spec.emit_rpc_server_typesupport(mod, "ibmw::demo")
        self.assertIsNotNone(ts_info)
        self.assertEqual(
            set(ts_info.keys()),
            {"msg_type_cpp", "inc_h_content", "inc_cc_content"},
        )
        self.assertEqual(
            ts_info["msg_type_cpp"],
            "example_ros2::srv::RosTestRpcSyncService",
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("#define IBMW_TS_RPC_SERVER_SETUP(core_expr)", inc_h)
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_DECL", inc_h)
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)", inc_h)
        self.assertIn("IBMW_TS_RPC_SERVER_HANDLER_IMPL(", inc_cc)

    def test_ros2_rpc_server_real_path_marker_contract(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "ros2",
            "ros2_package": "example_ros2",
            "ros2_service": "RosTestRpc",
            "_has_ros2_service_info": True,
        }

        self.assertEqual(
            self.spec.rpc_server_markers,
            _canonical_rpc_server_markers(),
        )
        from transports import ros2

        ts_info = ros2._gen_ros2_rpc_server_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_cc = ts_info["inc_cc_content"]
        method_marker = _method_marker(self.spec.rpc_server_markers[0], "RosTestRpc")
        begin = f"// {method_marker}_BEGIN"
        end = f"// {method_marker}_END"

        self.assertIn(begin, inc_cc)
        self.assertIn(end, inc_cc)
        self.assertLess(inc_cc.index(begin), inc_cc.index(end))
        self.assertIn("template <typename ServerT>", inc_cc)
        self.assertIn("return owner_->HandleRosTestRpc(ctx, req, rsp);", inc_cc)
        self.assertIn("return _ibmw_ros2_rpc_server_marshalling_helper_ros_test_rpc(", inc_cc)
        self.assertIn("rsp.code = 0;", inc_cc)

    def test_ros2_rpc_server_imported_srv_adapter_shape(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "ros2",
            "ros2_package": "robot_interfaces",
            "ros2_service": "BatchEcho",
            "_has_ros2_service_info": True,
            "ros2_imported_srv_adapter": True,
        }

        from transports import ros2

        ts_info = ros2._gen_ros2_rpc_server_markerized_typesupport(
            mod, "ibmw::demo"
        )
        self.assertEqual(
            ts_info["msg_type_cpp"],
            "ibmw::ros2_interfaces::robot_interfaces::BatchEchoSyncServiceAdapter",
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("#include <robot_interfaces/ibmw_rpc_bridge_runtime.hpp>", inc_h)
        self.assertIn(
            "public ibmw::ros2_interfaces::robot_interfaces::BatchEchoSyncServiceAdapter",
            inc_h,
        )
        self.assertIn("ibmw::rpc::Status HandleBatchEcho(", inc_h)
        self.assertIn(
            "const ibmw::ros2_interfaces::robot_interfaces::BatchEchoRpcBridgeRuntime::Request& req",
            inc_h,
        )
        self.assertIn(
            "ibmw::ros2_interfaces::robot_interfaces::BatchEchoRpcBridgeRuntime::Response& rsp",
            inc_h,
        )
        self.assertIn("RegisterService(service_ptr_.get())", inc_h)
        self.assertIn("field-agnostic", inc_cc)
        self.assertIn("IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED", inc_cc)
        self.assertNotIn("rsp.code", inc_cc)
        self.assertIn("return owner_->HandleBatchEcho(ctx, req, rsp);", inc_cc)

    def test_emit_ros2_rpc_client_real_thread_form(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "ros2",
            "ros2_package": "example_ros2",
            "ros2_service": "RosTestRpc",
            "_has_ros2_service_info": True,
        }

        self.assertIsNotNone(
            getattr(self.spec, "emit_rpc_client_typesupport", None),
            "ros2 plugin emit_rpc_client_typesupport hook missing — "
            "unified RPC client hook not wired",
        )

        ts_info = self.spec.emit_rpc_client_typesupport(mod, "ibmw::demo")
        self.assertIsNotNone(ts_info)
        self.assertEqual(
            set(ts_info.keys()),
            {"msg_type_cpp", "inc_h_content", "inc_cc_content"},
        )
        self.assertEqual(
            ts_info["msg_type_cpp"],
            "example_ros2::srv::RosTestRpcSyncAccess",
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("#define IBMW_TS_RPC_CLIENT_LOOP_DECL", inc_h)
        self.assertIn("#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)", inc_h)
        self.assertIn("thread-form RpcLoop helper", inc_cc)

    def test_ros2_rpc_client_real_path_marker_contract(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "ros2",
            "ros2_package": "example_ros2",
            "ros2_service": "RosTestRpc",
            "_has_ros2_service_info": True,
        }

        self.assertEqual(
            self.spec.rpc_client_markers,
            _canonical_rpc_client_markers(),
        )
        from transports import ros2

        ts_info = ros2._gen_ros2_rpc_client_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        request_marker = _method_marker(self.spec.rpc_client_markers[0], "RosTestRpc")
        response_marker = _method_marker(self.spec.rpc_client_markers[1], "RosTestRpc")
        status_marker = _method_marker(self.spec.rpc_client_markers[2], "RosTestRpc")

        request_begin = f"// {request_marker}_BEGIN"
        request_end = f"// {request_marker}_END"
        response_begin = f"// {response_marker}_BEGIN"
        response_end = f"// {response_marker}_END"
        status_begin = f"// {status_marker}_BEGIN"
        status_end = f"// {status_marker}_END"
        call_anchor = "auto status = self->access_->RosTestRpc(ctx_ptr, req, rsp);"

        for begin, end in (
            (request_begin, request_end),
            (response_begin, response_end),
            (status_begin, status_end),
        ):
            self.assertIn(begin, inc_cc)
            self.assertIn(end, inc_cc)
            self.assertLess(inc_cc.index(begin), inc_cc.index(end))

        self.assertIn(call_anchor, inc_cc)
        self.assertLess(inc_cc.index(request_end), inc_cc.index(call_anchor))
        self.assertIn("void RpcLoop();", inc_h)
        self.assertNotIn("co_await", inc_cc)
        self.assertIn("std::this_thread::sleep_for", inc_cc)

    def test_ros2_rpc_client_imported_srv_adapter_shape(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "ros2",
            "ros2_package": "robot_interfaces",
            "ros2_service": "BatchEcho",
            "_has_ros2_service_info": True,
            "ros2_imported_srv_adapter": True,
        }

        from transports import ros2

        ts_info = ros2._gen_ros2_rpc_client_markerized_typesupport(
            mod, "ibmw::demo"
        )
        self.assertEqual(
            ts_info["msg_type_cpp"],
            "ibmw::ros2_interfaces::robot_interfaces::BatchEchoSyncClientAdapter",
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("#include <robot_interfaces/ibmw_rpc_bridge_runtime.hpp>", inc_h)
        self.assertIn(
            "std::shared_ptr<ibmw::ros2_interfaces::robot_interfaces::BatchEchoSyncClientAdapter> access_",
            inc_h,
        )
        self.assertIn(
            "ibmw::ros2_interfaces::robot_interfaces::BatchEchoRpcBridgeRuntime::Request req;",
            inc_cc,
        )
        self.assertIn(
            "ibmw::ros2_interfaces::robot_interfaces::BatchEchoRpcBridgeRuntime::Response rsp;",
            inc_cc,
        )
        self.assertIn("auto status = self->access_->BatchEcho(ctx_ptr, req, rsp);", inc_cc)
        self.assertIn("robot_interfaces::srv::to_yaml(req)", inc_cc)

    def test_ros2_rpc_client_imported_setup_body(self):
        from generators import module_gen

        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "ros2",
            "ros2_package": "robot_interfaces",
            "ros2_service": "BatchEcho",
            "ros2_imported_srv_adapter": True,
        }
        body = module_gen._gen_rpc_client_setup_body(mod)
        self.assertIn(
            "ibmw::ros2_interfaces::robot_interfaces::RegisterBatchEchoClientFunc(rpc_handle)",
            body,
        )
        self.assertIn(
            "std::make_shared<ibmw::ros2_interfaces::robot_interfaces::BatchEchoSyncClientAdapter>(rpc_handle)",
            body,
        )
        self.assertNotIn("robot_interfaces::srv::RegisterBatchEchoClientFunc", body)
        self.assertNotIn("robot_interfaces::srv::BatchEchoSyncAccess", body)

    def test_emit_returns_none_for_unknown_type(self):
        mod = {
            "name": "x",
            "type": "totally_made_up_type",
            "transport": "ros2",
        }
        self.assertIsNone(self.spec.emit_module(mod, "ns", "x_module"))


if __name__ == "__main__":
    unittest.main()
