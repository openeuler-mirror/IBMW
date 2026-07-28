#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DDS plugin emit_module contract tests.
#
# Verifies that:
#   1. The dds plugin is registered via transports.get("dds").
#   2. Its emit_module callable exists.
#   3. Plugin's `owns_type_keys` covers every dds-flavoured module type
#      we expect (publisher, subscriber, rpc_*, direct_ros2_*,
#      ros2_interop_*, loaned_publisher, loaned_subscriber).
#   4. Plugin-emitted bytes for a synthetic publisher dict are stable
#      and shaped like real generated code (header + cc, copyright
#      banner, expected #include directives). Byte-equality to legacy
#      `module_gen._GENERATORS` is no longer applicable because plugin
#      moved every dds generator body out of module_gen and into
#      transports/dds.py — the plugin IS the source of truth.
#   5. emit_module returns None for an unknown type (fall-through
#      contract).
#
# Usage:
#   python3 -m pytest tests/test_dds_plugin_emit.py -v

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]


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


sys.path.insert(0, str(ROOT))

# Every dds-flavoured module type the dds plugin must own. Derived from
# the original `_GENERATORS` table prior to plugin extraction.
EXPECTED_DDS_TYPE_KEYS = {
    "publisher",
    "subscriber",
    "rpc_server",
    "rpc_client",
    "direct_ros2_publisher",
    "direct_ros2_subscriber",
    "ros2_interop_publisher",
    "ros2_interop_subscriber",
    "loaned_publisher",
    "loaned_subscriber",
}


class TestDdsPluginEmit(unittest.TestCase):
    def setUp(self):
        import transports
        from generators import module_gen

        self.transports = transports
        self.module_gen = module_gen
        self.spec = transports.get("dds")

    def test_dds_plugin_registered(self):
        self.assertIsNotNone(self.spec, "dds plugin not registered")
        self.assertEqual(self.spec.name, "dds")

    def test_emit_module_callable(self):
        self.assertIsNotNone(
            self.spec.emit_module,
            "dds plugin emit_module is None — W4.1 not wired",
        )

    def test_owns_all_expected_dds_type_keys(self):
        owned = set(self.spec.meta.get("owns_type_keys", []))
        missing = EXPECTED_DDS_TYPE_KEYS - owned
        self.assertFalse(
            missing,
            f"plugin does not claim these dds types: {sorted(missing)}",
        )

    def test_module_gen_no_longer_has_dds_keys(self):
        # The legacy table must not contain any (*, "dds") entries —
        # those generators all live in transports/dds.py now.
        leftover = {
            (t, tr) for (t, tr) in self.module_gen._GENERATORS
            if tr == "dds"
        }
        self.assertFalse(
            leftover,
            f"module_gen._GENERATORS still has dds keys after plugin extraction: "
            f"{sorted(leftover)} — these must move to transports/dds.py",
        )

    def test_emit_publisher_shape_and_stability(self):
        # Publisher now flows through the unified
        # path — dds plugin exposes emit_publisher_typesupport and
        # returns None from emit_module for publisher. Exercise the
        # new contract directly.
        mod = {
            "name": "demo_pub",
            "type": "publisher",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_dds_types_header": "demo_dds_types.h",
            "message_type": "DemoMsg",
            "topic": "/demo",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_pub_module"

        # emit_module now defers publisher to unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_publisher_typesupport produces the transport-specific
        # half. Confirm shape + that the fully-qualified message type
        # survives (dds_ns::MsgType).
        self.assertIsNotNone(
            getattr(self.spec, "emit_publisher_typesupport", None),
            "dds plugin emit_publisher_typesupport hook missing — "
            "unified publisher hook not wired",
        )
        ts_info = self.spec.emit_publisher_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"],
                         "ibmw::protocols::demo::DemoMsg")
        self.assertIn("demo_dds_types.h", ts_info["inc_h_content"])
        self.assertIn("type_support/dds/dds_transport.h",
                      ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_SENDER_ACCESS_MEMBER",
                      ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_SENDER_ACCESS_INIT",
                      ts_info["inc_h_content"])
        self.assertIn("access_->Send(msg);", ts_info["publish_stmt"])

        # Now exercise the unified driver end-to-end (module_gen entry
        # point) to prove the 4-file emission works.
        project = {
            "project": {"_namespace_cpp": ns},
            "modules": [dict(mod)],
        }
        all_out = self.module_gen.generate_modules(project)
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
        # R1 guard: user-facing header must not include any transport
        # bridge header.
        user_h = out_by_path[f"src/{mod_dir}/demo_pub.h"]
        self.assertNotIn("dds_types/transport/dds_transport.h", user_h)
        self.assertNotIn("rclcpp/rclcpp.hpp", user_h)
        self.assertIn("demo_pub_typesupport.inc.h", user_h)
        # R2 guard: class body must not name any transport-namespaced
        # concrete SenderAccess specialization (it now goes through
        # the IBMW_TS_SENDER_ACCESS_MEMBER macro).
        self.assertNotIn("SenderAccess<ibmw::protocols::demo::DemoMsg>",
                         user_h)
        self.assertIn("IBMW_TS_SENDER_ACCESS_MEMBER access_;", user_h)

    def test_emit_subscriber_shape_and_stability(self):
        # Subscriber flows through the unified path.
        mod = {
            "name": "demo_sub",
            "type": "subscriber",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_dds_types_header": "demo_dds_types.h",
            "message_type": "DemoMsg",
            "topic": "/demo",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_sub_module"

        # emit_module defers subscriber to unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_subscriber_typesupport shape.
        self.assertIsNotNone(
            getattr(self.spec, "emit_subscriber_typesupport", None),
            "dds plugin emit_subscriber_typesupport hook missing — "
            "unified subscriber hook not wired",
        )
        ts_info = self.spec.emit_subscriber_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"],
                         "ibmw::protocols::demo::DemoMsg")
        self.assertIn("demo_dds_types.h", ts_info["inc_h_content"])
        self.assertIn("type_support/dds/dds_transport.h",
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
        # R1: no dds transport / dds types headers in user-facing .h.
        self.assertNotIn("dds_types/transport/dds_transport.h", user_h)
        self.assertNotIn("demo_dds_types.h", user_h)
        self.assertIn("demo_sub_typesupport.inc.h", user_h)
        # R2: no ReceiverAccess<ConcreteType> in main .cc.
        user_cc = out_by_path[f"src/{mod_dir}/demo_sub.cc"]
        self.assertNotIn(
            "ReceiverAccess<ibmw::protocols::demo::DemoMsg>", user_cc)
        self.assertIn("IBMW_TS_RECEIVER_ACCESS_SETUP", user_cc)
        self.assertIn("on_message_body", user_cc)

    def test_emit_returns_none_for_unknown_type(self):
        mod = {
            "name": "x",
            "type": "totally_made_up_type",
            "transport": "dds",
        }
        self.assertIsNone(self.spec.emit_module(mod, "ns", "x_module"))

    def test_emit_loaned_subscriber_shape(self):
        # loaned_subscriber flows through the unified
        # path via emit_loaned_subscriber_typesupport. emit_module returns
        # None; module_gen unified emitter produces the 4 files.
        mod = {
            "name": "demo_loan_sub",
            "type": "loaned_subscriber",
            "transport": "dds",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_type_name": "geometry_msgs/msg/Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/demo_loan",
            "subscriber_style": "callback",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_loan_sub_module"

        # emit_module defers loaned_subscriber to the unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_loaned_subscriber_typesupport shape.
        self.assertIsNotNone(
            getattr(self.spec, "emit_loaned_subscriber_typesupport", None),
            "dds plugin emit_loaned_subscriber_typesupport hook missing — "
            "unified loaned-subscriber hook not wired",
        )
        ts_info = self.spec.emit_loaned_subscriber_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"], "geometry_msgs::msg::Point")
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        # inc.h carries the transport-specific includes and macro contract.
        self.assertIn("type_support/ros2/ros2_transport.h", inc_h)
        self.assertIn("geometry_msgs/msg/point.hpp", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_MSG_TYPE", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_STATIC_ASSERT", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_ATTACH", inc_h)
        # dds uses shared_ptr<const T> signature, not (const T&, size_t).
        self.assertIn("std::shared_ptr<", inc_h)
        self.assertIn("ibmw::ros2::transport::OnMessage<", inc_h)
        # inc.cc carries the default OnMessage body with the user-code marker.
        self.assertIn("on_message_body", inc_cc)
        self.assertIn("std::shared_ptr<const geometry_msgs::msg::Point>",
                      inc_cc)

        # Unified end-to-end: 4 files + R1/R2/R3 guards.
        project = {
            "project": {"_namespace_cpp": ns},
            "modules": [dict(mod)],
        }
        all_out = self.module_gen.generate_modules(project)
        paths = {p for p, _ in all_out}
        self.assertEqual(
            paths,
            {
                f"src/{mod_dir}/demo_loan_sub.h",
                f"src/{mod_dir}/demo_loan_sub.cc",
                f"src/{mod_dir}/demo_loan_sub_typesupport.inc.h",
                f"src/{mod_dir}/demo_loan_sub_typesupport.inc.cc",
            },
        )
        out_by_path = dict(all_out)
        user_h = out_by_path[f"src/{mod_dir}/demo_loan_sub.h"]
        user_cc = out_by_path[f"src/{mod_dir}/demo_loan_sub.cc"]
        # R1: no dds / ros2-transport headers in the user-facing .h.
        self.assertNotIn("ros2_types/transport/ros2_transport.h", user_h)
        self.assertNotIn("geometry_msgs/msg/point.hpp", user_h)
        self.assertIn("demo_loan_sub_typesupport.inc.h", user_h)
        # R2: no ros2::transport::OnMessage<ConcreteType> in main .cc.
        self.assertNotIn(
            "OnMessage<geometry_msgs::msg::Point>", user_cc)
        self.assertIn("IBMW_TS_LOANED_SUB_ATTACH", user_cc)
        # R3: user.cc includes the transport-specific inc.cc TU fragment.
        self.assertIn("demo_loan_sub_typesupport.inc.cc", user_cc)

    def test_emit_loaned_publisher_shape(self):
        # loaned_publisher flows through the unified
        # path via emit_loaned_publisher_typesupport. emit_module returns
        # None; module_gen unified emitter produces the 4 files.
        mod = {
            "name": "demo_loan_pub",
            "type": "loaned_publisher",
            "transport": "dds",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_type_name": "geometry_msgs/msg/Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/demo_loan",
            "dds_type_name": "geometry_msgs/msg/Point",
            "zero_copy_config": "strict",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_loan_pub_module"

        # emit_module defers loaned_publisher to the unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_loaned_publisher_typesupport hook is wired.
        self.assertIsNotNone(
            getattr(self.spec, "emit_loaned_publisher_typesupport", None),
            "dds plugin emit_loaned_publisher_typesupport hook missing — "
            "unified loaned-publisher hook not wired",
        )
        ts_info = self.spec.emit_loaned_publisher_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"], "geometry_msgs::msg::Point")
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]

        # The sidecar binds only to stable public APIs.
        self.assertIn("cpp/transport/transport_factory.h", inc_h)
        self.assertIn("type_support/ros2/ros2_transport.h", inc_h)
        self.assertIn("geometry_msgs/msg/point.hpp", inc_h)
        for macro in (
            "IBMW_TS_LOANED_PUB_MSG_TYPE",
            "IBMW_TS_LOANED_PUB_STATIC_ASSERT",
            "IBMW_TS_LOANED_PUB_MEMBER",
            "IBMW_TS_LOANED_PUB_SETUP",
            "IBMW_TS_LOANED_PUB_RUN_INIT",
            "IBMW_TS_LOANED_PUB_HOT_LOOP_BODY",
            "IBMW_TS_LOANED_PUB_PRINT_STATS",
        ):
            self.assertIn(f"#define {macro}", inc_h,
                          f"inc.h must define {macro}")
        self.assertIn("SenderAccess<", inc_h)
        self.assertIn("publisher_->Loan()", inc_h)
        self.assertIn("publisher_->Publish", inc_h)
        self.assertNotIn("dds_ext/", inc_h)
        self.assertNotIn("DdsTransportDriver", inc_h)
        self.assertNotIn("SetDdsDriver", inc_h)
        self.assertNotIn("SetDriverSlot(", inc_h)
        self.assertNotIn("Ros2StylePublisher<", inc_h)
        self.assertNotIn("create_publisher_with_driver_config<", inc_h)
        # Hot-loop user-code marker pair lives inside the macro.
        self.assertIn("populate_loaned_message", inc_h)
        # Strict public loans require in-place-safe message storage.
        self.assertIn("std::is_trivially_copyable_v<", inc_h)

        # Unified end-to-end: 4 files + R1/R2/R9 guards.
        project = {
            "project": {"_namespace_cpp": ns},
            "modules": [dict(mod)],
        }
        all_out = self.module_gen.generate_modules(project)
        paths = {p for p, _ in all_out}
        self.assertEqual(
            paths,
            {
                f"src/{mod_dir}/demo_loan_pub.h",
                f"src/{mod_dir}/demo_loan_pub.cc",
                f"src/{mod_dir}/demo_loan_pub_typesupport.inc.h",
                f"src/{mod_dir}/demo_loan_pub_typesupport.inc.cc",
            },
        )
        out_by_path = dict(all_out)
        user_h = out_by_path[f"src/{mod_dir}/demo_loan_pub.h"]
        user_cc = out_by_path[f"src/{mod_dir}/demo_loan_pub.cc"]
        # R1: no transport / driver headers in user-facing .h.
        self.assertNotIn("dds_ext/", user_h)
        self.assertNotIn("ros2_types/transport/ros2_transport.h", user_h)
        self.assertNotIn("dds_transport_driver", user_h)
        self.assertNotIn("geometry_msgs/msg/point.hpp", user_h)
        self.assertIn("demo_loan_pub_typesupport.inc.h", user_h)
        # R2: no concrete Ros2StylePublisher<T> / create_publisher_*<T> in
        # user .cc.
        self.assertNotIn(
            "Ros2StylePublisher<geometry_msgs::msg::Point>", user_cc)
        self.assertNotIn(
            "create_publisher_with_driver_config<", user_cc)
        self.assertIn("IBMW_TS_LOANED_PUB_HOT_LOOP_BODY", user_cc)
        self.assertIn("IBMW_TS_LOANED_PUB_SETUP", user_cc)
        self.assertIn("IBMW_TS_LOANED_PUB_RUN_INIT", user_cc)
        # R9: user.cc includes the inc.cc TU fragment.
        self.assertIn("demo_loan_pub_typesupport.inc.cc", user_cc)

    def test_emit_dds_rpc_server_typesupport_shape(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        self.assertIsNotNone(
            getattr(self.spec, "emit_rpc_server_typesupport", None),
            "dds plugin emit_rpc_server_typesupport hook missing — "
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
            "ibmw::protocols::demo::CalculatorServiceCoServer",
        )
        self.assertIn("demo_rpc.ibmw_rpc.dds.h", ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RPC_SERVER_SETUP", ts_info["inc_h_content"])
        self.assertIn(
            "IBMW_TS_RPC_SERVER_HANDLER_DECL", ts_info["inc_h_content"])
        self.assertIn(
            "IBMW_TS_RPC_SERVER_HANDLER_IMPL", ts_info["inc_h_content"])
        self.assertIn(
            "IBMW_TS_RPC_SERVER_HANDLER_IMPL", ts_info["inc_cc_content"])

    def test_emit_dds_rpc_server_macro_contract(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        inc_h = self.spec.emit_rpc_server_typesupport(
            mod, "ibmw::demo")["inc_h_content"]
        self.assertIn("#define IBMW_TS_RPC_SERVER_SETUP(", inc_h)
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_DECL", inc_h)
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(", inc_h)

    def test_dds_rpc_server_marker_presence(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        self.assertEqual(self.spec.rpc_server_markers, _canonical_rpc_server_markers())
        from transports import dds

        ts_info = dds.emit_dds_rpc_server_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_cc = ts_info["inc_cc_content"]
        for marker in self.spec.rpc_server_markers:
            method_marker = _method_marker(marker, "Add")
            begin = f"// {method_marker}_BEGIN"
            end = f"// {method_marker}_END"
            self.assertIn(begin, inc_cc)
            self.assertIn(end, inc_cc)
            self.assertLess(inc_cc.index(begin), inc_cc.index(end))
        self.assertIn("return self->OnAdd(ctx, req, rsp);", inc_cc)

    def test_dds_rpc_server_marshalling_helper_shape(self):
        mod = {
            "name": "demo_rpc_server",
            "type": "rpc_server",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        from transports import dds

        ts_info = dds.emit_dds_rpc_server_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("server_handle->Add.ServeInline(", inc_h)
        self.assertIn(
            "_ibmw_dds_rpc_server_marshalling_helper_demo_rpc_server_add",
            inc_h,
        )
        self.assertIn("template <typename ServerT>", inc_cc)
        self.assertIn(
            "inline ibmw::rpc::Status "
            "_ibmw_dds_rpc_server_marshalling_helper_demo_rpc_server_add(",
            inc_cc,
        )
        self.assertIn("ServerT* self", inc_cc)
        self.assertIn("const ibmw::protocols::demo::Add_Request& req", inc_cc)
        self.assertIn("ibmw::protocols::demo::Add_Response& rsp", inc_cc)

    def test_emit_dds_rpc_client_typesupport_shape(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        self.assertIsNotNone(
            getattr(self.spec, "emit_rpc_client_typesupport", None),
            "dds plugin emit_rpc_client_typesupport hook missing — "
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
            "ibmw::protocols::demo::CalculatorServiceCoClient",
        )
        self.assertIn("demo_rpc.ibmw_rpc.dds.h", ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RPC_CLIENT_LOOP_DECL",
                      ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RPC_CLIENT_LOOP_IMPL",
                      ts_info["inc_h_content"])

    def test_dds_rpc_client_request_marker_presence(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        self.assertEqual(self.spec.rpc_client_markers, _canonical_rpc_client_markers())
        from transports import dds

        ts_info = dds.emit_dds_rpc_client_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_cc = ts_info["inc_cc_content"]
        request_marker = _method_marker(self.spec.rpc_client_markers[0], "Add")
        begin = f"// {request_marker}_BEGIN"
        end = f"// {request_marker}_END"
        call_anchor = "co_await self->client_.Add.Call(req, rsp);"

        self.assertIn(begin, inc_cc)
        self.assertIn(end, inc_cc)
        self.assertIn(call_anchor, inc_cc)
        self.assertLess(inc_cc.index(begin), inc_cc.index(end))
        self.assertLess(inc_cc.index(end), inc_cc.index(call_anchor))

    def test_dds_rpc_client_response_status_marker_under_coroutine(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        from transports import dds

        ts_info = dds.emit_dds_rpc_client_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        response_marker = _method_marker(self.spec.rpc_client_markers[1], "Add")
        status_marker = _method_marker(self.spec.rpc_client_markers[2], "Add")

        for marker in (response_marker, status_marker):
            begin = f"// {marker}_BEGIN"
            end = f"// {marker}_END"
            self.assertIn(begin, inc_cc)
            self.assertIn(end, inc_cc)
            self.assertLess(inc_cc.index(begin), inc_cc.index(end))

        self.assertIn("ibmw::co::Task<void> RpcLoop();", inc_h)
        self.assertIn(
            "inline ibmw::co::Task<void> "
            "_ibmw_dds_rpc_client_loop_body_impl_demo_rpc_client(SelfT* self)",
            inc_cc,
        )
        self.assertIn("co_await self->client_.Add.Call(req, rsp);", inc_cc)
        self.assertIn("co_await ibmw::co::ScheduleAfter(", inc_cc)


    def test_emit_dds_rpc_client_coroutine_contract(self):
        mod = {
            "name": "demo_rpc_client",
            "type": "rpc_client",
            "transport": "dds",
            "_dds_namespace_cpp": "ibmw::protocols::demo",
            "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
            "_dds_service_info": {
                "name": "CalculatorService",
                "methods": [{
                    "name": "Add",
                    "request": "Add_Request",
                    "response": "Add_Response",
                }],
            },
        }

        inc_h = self.spec.emit_rpc_client_typesupport(
            mod, "ibmw::demo")["inc_h_content"]
        self.assertIn("co::Task<void>", inc_h)
        self.assertIn("co_await", inc_h)


if __name__ == "__main__":
    unittest.main()
