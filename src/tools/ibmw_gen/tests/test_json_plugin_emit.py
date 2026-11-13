#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# JSON plugin emit_module contract tests.
#
# Mirrors test_iceoryx_plugin_emit.py / test_ros2_plugin_emit.py. Verifies:
#   1. json plugin registered via transports.get("json").
#   2. emit_module callable exists rather than a None stub.
#   3. Plugin `owns_type_keys` covers publisher/subscriber/rpc_server/rpc_client.
#   4. module_gen._GENERATORS no longer contains any (*, "json") row.
#   5. Plugin-emitted bytes for synthetic publisher/subscriber/rpc_server dicts
#      are shaped like real generated code.
#   6. emit_module returns None for unknown type (fall-through contract).
#   7. plugin meta preserves kind="serialization" (schema/validator parity).
#
# Usage:
#   python3 -m pytest tests/test_json_plugin_emit.py -v

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from importlib import import_module
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
IBMW_GEN_CLI = ROOT / "ibmw_gen.py"
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

EXPECTED_JSON_TYPE_KEYS = {
    "publisher",
    "subscriber",
    "rpc_server",
    "rpc_client",
}


class TestJsonPluginEmit(unittest.TestCase):
    transports: Any = None
    module_gen: Any = None
    spec: Any = None

    def setUp(self):
        self.transports = import_module("transports")
        self.module_gen = import_module("generators.module_gen")
        self.spec = self.transports.get("json")

    def test_json_plugin_registered(self):
        self.assertIsNotNone(self.spec, "json plugin not registered")
        self.assertEqual(self.spec.name, "json")

    def test_emit_module_callable(self):
        self.assertIsNotNone(
            self.spec.emit_module,
            "json plugin emit_module is not wired",
        )

    def test_meta_preserves_serialization_kind(self):
        self.assertEqual(self.spec.meta.get("kind"), "serialization")

    def test_owns_all_expected_json_type_keys(self):
        owned = set(self.spec.meta.get("owns_type_keys", []))
        missing = EXPECTED_JSON_TYPE_KEYS - owned
        self.assertFalse(
            missing,
            f"plugin does not claim these json types: {sorted(missing)}",
        )

    def test_module_gen_no_longer_has_json_keys(self):
        # The legacy table must not contain any (*, "json") entries —
        # those generators all live in transports/json.py now.
        leftover = {
            (t, tr) for (t, tr) in self.module_gen._GENERATORS
            if tr == "json"
        }
        self.assertFalse(
            leftover,
            (
                "module_gen._GENERATORS still has json keys after plugin extraction: "
                f"{sorted(leftover)} — these must move to transports/json.py"
            ),
        )

    def test_emit_publisher_shape_and_stability(self):
        # Publisher now uses the unified path.
        mod = {
            "name": "demo_json_pub",
            "type": "publisher",
            "transport": "json",
            "message_type": "demo::CustomType",
            "topic": "demo_topic",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_json_pub_module"

        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        ts_info = self.spec.emit_publisher_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"], "demo::CustomType")
        self.assertIn("cpp/util/json_transport.h", ts_info["inc_h_content"])
        self.assertIn("json_custom_types.h", ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_SENDER_ACCESS_MEMBER",
                      ts_info["inc_h_content"])
        # Stability: identical on repeated call.
        self.assertEqual(ts_info,
                         self.spec.emit_publisher_typesupport(mod, ns))

        # End-to-end unified path.
        project = {"project": {"_namespace_cpp": ns}, "modules": [dict(mod)]}
        all_out = self.module_gen.generate_modules(project)
        paths = {p for p, _ in all_out}
        self.assertEqual(
            paths,
            {
                f"src/{mod_dir}/demo_json_pub.h",
                f"src/{mod_dir}/demo_json_pub.cc",
                f"src/{mod_dir}/demo_json_pub_typesupport.inc.h",
                f"src/{mod_dir}/demo_json_pub_typesupport.inc.cc",
            },
        )
        out_by_path = dict(all_out)
        user_h = out_by_path[f"src/{mod_dir}/demo_json_pub.h"]
        # R1/R2 guards.
        self.assertNotIn("json_transport.h", user_h)
        self.assertNotIn("SenderAccess<demo::CustomType>", user_h)

    def test_emit_subscriber_shape(self):
        # Subscriber now flows through the unified
        # path — json plugin exposes emit_subscriber_typesupport and
        # returns None from emit_module for subscriber.
        mod = {
            "name": "demo_json_sub",
            "type": "subscriber",
            "transport": "json",
            "message_type": "demo::CustomType",
            "topic": "demo_topic",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_json_sub_module"

        # emit_module defers subscriber to unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_subscriber_typesupport produces the transport-specific half.
        self.assertIsNotNone(
            getattr(self.spec, "emit_subscriber_typesupport", None),
            "json plugin emit_subscriber_typesupport hook missing",
        )
        ts_info = self.spec.emit_subscriber_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"], "demo::CustomType")
        self.assertIn("json_custom_types.h", ts_info["inc_h_content"])
        self.assertIn("cpp/util/json_transport.h", ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RECEIVER_MSG_TYPE",
                      ts_info["inc_h_content"])
        self.assertIn("IBMW_TS_RECEIVER_ACCESS_SETUP",
                      ts_info["inc_h_content"])

        # Exercise the unified driver end-to-end (4-file emission).
        project = {
            "project": {"_namespace_cpp": ns},
            "modules": [dict(mod)],
        }
        all_out = self.module_gen.generate_modules(project)
        paths = {p for p, _ in all_out}
        self.assertEqual(
            paths,
            {
                f"src/{mod_dir}/demo_json_sub.h",
                f"src/{mod_dir}/demo_json_sub.cc",
                f"src/{mod_dir}/demo_json_sub_typesupport.inc.h",
                f"src/{mod_dir}/demo_json_sub_typesupport.inc.cc",
            },
        )
        out_by_path = dict(all_out)
        # R1 guard: user-facing header must not include transport headers.
        user_h = out_by_path[f"src/{mod_dir}/demo_json_sub.h"]
        self.assertNotIn("cpp/util/json_transport.h", user_h)
        self.assertNotIn("json_custom_types.h", user_h)
        self.assertIn("demo_json_sub_typesupport.inc.h", user_h)
        # R2 guard: main .cc must not name ReceiverAccess<ConcreteType>
        # directly — it goes through IBMW_TS_RECEIVER_MSG_TYPE macro.
        user_cc = out_by_path[f"src/{mod_dir}/demo_json_sub.cc"]
        self.assertNotIn("ReceiverAccess<demo::CustomType>", user_cc)
        self.assertIn("IBMW_TS_RECEIVER_ACCESS_SETUP", user_cc)
        self.assertIn("IBMW_TS_RECEIVER_MSG_TYPE", user_cc)
        self.assertIn("on_message_body", user_cc)

    def test_json_pubsub_accepts_imported_types_header(self):
        pub_mod = {
            "name": "import_json_pub",
            "type": "publisher",
            "transport": "json",
            "message_type": "ibmw::ros2_interfaces::json::demo::Plain",
            "topic": "/plain",
            "ros2_import_json_types_header": "demo_json_types.hpp",
        }
        sub_mod = {
            "name": "import_json_sub",
            "type": "subscriber",
            "transport": "json",
            "message_type": "ibmw::ros2_interfaces::json::demo::Plain",
            "topic": "/plain",
            "ros2_import_json_types_header": "demo_json_types.hpp",
        }

        pub_ts = self.spec.emit_publisher_typesupport(pub_mod, "ibmw::demo")
        sub_ts = self.spec.emit_subscriber_typesupport(sub_mod, "ibmw::demo")

        self.assertIn('#include "demo_json_types.hpp"', pub_ts["inc_h_content"])
        self.assertIn('#include "demo_json_types.hpp"', sub_ts["inc_h_content"])
        self.assertNotIn('#include "json_custom_types.h"', pub_ts["inc_h_content"])
        self.assertNotIn('#include "json_custom_types.h"', sub_ts["inc_h_content"])
        self.assertLess(
            pub_ts["inc_h_content"].index('#include "demo_json_types.hpp"'),
            pub_ts["inc_h_content"].index('#include "cpp/util/json_transport.h"'),
        )
        self.assertLess(
            sub_ts["inc_h_content"].index('#include "demo_json_types.hpp"'),
            sub_ts["inc_h_content"].index('#include "cpp/util/json_transport.h"'),
        )

    def test_cli_json_project_without_json_types_emits_placeholder_header(self):
        tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_json_no_types_"))
        try:
            yaml_path = tmpdir / "json_no_types.ibmw.yaml"
            out_dir = tmpdir / "json_no_types_gen"
            yaml_path.write_text(
                """\
project:
  name: json_no_types
  namespace: ibmw::demo

modules:
  - name: PlainJsonPublisher
    type: publisher
    transport: json
    message_type: ibmw::demo::Plain
    topic: /plain

apps: []
""",
                encoding="utf-8",
            )

            rc = subprocess.run(
                [sys.executable, str(IBMW_GEN_CLI), str(yaml_path), "-o", str(out_dir)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(
                rc.returncode, 0,
                f"ibmw-gen failed:\nstdout={rc.stdout}\nstderr={rc.stderr}",
            )

            header = out_dir / "src" / "json_custom_types.h"
            self.assertTrue(header.exists(), "missing placeholder json_custom_types.h")
            header_text = header.read_text(encoding="utf-8")
            self.assertIn("#pragma once", header_text)
            self.assertIn("namespace ibmw::demo", header_text)
            self.assertIn(
                "target_include_directories(plain_json_publisher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)",
                (out_dir / "CMakeLists.txt").read_text(encoding="utf-8"),
            )
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def test_emit_rpc_server_shape(self):
        # plugin emit_module no longer handles rpc_server —
        # the unified path in module_gen._gen_rpc_server_unified handles all
        # rpc emission via _gen_json_rpc_server_markerized_typesupport
        # marker-preserving sidecar protocol. Plugin must defer (return None) for
        # rpc_server, mirroring the publisher/subscriber contract above.
        # Real-path shape coverage lives in
        # test_emit_json_rpc_server_typesupport_shape and
        # test_json_rpc_server_method_scoped_marker_contract below.
        mod = {
            "name": "demo_json_rpc_server",
            "type": "rpc_server",
            "transport": "json",
            "service_name": "DemoService",
            "request_type": "demo::Req",
            "response_type": "demo::Resp",
        }
        self.assertIsNone(
            self.spec.emit_module(mod, "ibmw::demo",
                                  "demo_json_rpc_server_module")
        )

    def test_emit_json_rpc_server_typesupport_shape(self):
        mod = {
            "name": "demo_json_rpc_server",
            "type": "rpc_server",
            "transport": "json",
            "service_name": "DemoService",
            "methods": [{
                "name": "Add",
                "request": "demo::Req",
                "response": "demo::Resp",
            }],
        }

        self.assertIsNotNone(
            getattr(self.spec, "emit_rpc_server_typesupport", None),
            "json plugin emit_rpc_server_typesupport hook missing",
        )

        ts_info = self.spec.emit_rpc_server_typesupport(mod, "ibmw::demo")
        self.assertIsNotNone(ts_info)
        self.assertEqual(
            set(ts_info.keys()),
            {"msg_type_cpp", "inc_h_content", "inc_cc_content"},
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("#define IBMW_TS_RPC_SERVER_SETUP(core_expr)", inc_h)
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_DECL", inc_h)
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)", inc_h)
        self.assertIn("IBMW_TS_RPC_SERVER_HANDLER_IMPL(", inc_cc)
        self.assertIn("std::unique_ptr<ibmw::rpc::ServiceBase> service_;", inc_h)
        self.assertIn("explicit _IbmwJsonRpcServerdemo_json_rpc_serverDemoServiceImpl(demo_json_rpc_server* owner);", inc_h)
        self.assertIn("namespace ibmw::demo {", inc_cc)
        self.assertIn("owner_->HandleAdd(req, rsp);", inc_cc)
        # GEN-001: handlers are plain functions; a // marker anchor inside a
        # backslash-continued macro would comment out the rest of it.
        self.assertIn("#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)\n", inc_cc)
        self.assertNotRegex(inc_cc, r"//[^\n]*\\\n")
        # TOOL-046: an unedited handler reports itself as unimplemented.
        self.assertIn("return IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED;", inc_cc)

    def test_json_rpc_server_method_scoped_marker_contract(self):
        mod = {
            "name": "demo_json_rpc_server",
            "type": "rpc_server",
            "transport": "json",
            "service_name": "DemoService",
            "methods": [
                {
                    "name": "AddTwoInts",
                    "request": "demo::AddReq",
                    "response": "demo::AddRsp",
                },
                {
                    "name": "ResetCounter",
                    "request": "demo::ResetReq",
                    "response": "demo::ResetRsp",
                },
            ],
        }

        self.assertEqual(
            self.spec.rpc_server_markers,
            _canonical_rpc_server_markers(),
        )
        json_transport = import_module("transports.json")
        ts_info = json_transport._gen_json_rpc_server_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_cc = ts_info["inc_cc_content"]

        for method_name in ("AddTwoInts", "ResetCounter"):
            method_marker = _method_marker(
                self.spec.rpc_server_markers[0], method_name
            )
            begin = f"// {method_marker}_BEGIN"
            end = f"// {method_marker}_END"
            handler_sig = f"uint32_t demo_json_rpc_server::Handle{method_name}("
            self.assertIn(begin, inc_cc)
            self.assertIn(end, inc_cc)
            self.assertIn(handler_sig, inc_cc)
            self.assertLess(inc_cc.index(begin), inc_cc.index(end))
            self.assertLess(inc_cc.index(handler_sig), inc_cc.index(begin))


    def test_emit_json_rpc_client_inc_h_thread_form(self):
        mod = {
            "name": "demo_json_rpc_client",
            "type": "rpc_client",
            "transport": "json",
            "service_name": "DemoService",
            "methods": [{
                "name": "Add",
                "request": "demo::Req",
                "response": "demo::Resp",
            }],
        }

        self.assertIsNotNone(
            getattr(self.spec, "emit_rpc_client_typesupport", None),
            "json plugin emit_rpc_client_typesupport hook missing",
        )

        ts_info = self.spec.emit_rpc_client_typesupport(mod, "ibmw::demo")
        self.assertIsNotNone(ts_info)
        self.assertEqual(
            set(ts_info.keys()),
            {"msg_type_cpp", "inc_h_content", "inc_cc_content"},
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        self.assertIn("#define IBMW_TS_RPC_CLIENT_LOOP_DECL", inc_h)
        self.assertIn("#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)", inc_h)
        self.assertIn("std::this_thread::sleep_for", inc_h)
        self.assertNotIn("co_await", inc_h)
        self.assertIn("thread-form RpcLoop helper", inc_cc)
        self.assertIn("namespace ibmw::demo {", inc_cc)
        self.assertIn("self->service_ref_, kServiceName,", inc_cc)
        self.assertIn("MW_LOG(self->GetLogger(), ibmw::common::util::kLogLevelInfo", inc_cc)
        self.assertIn("MW_LOG(self->GetLogger(), ibmw::common::util::kLogLevelWarn", inc_cc)

    def test_json_rpc_native_json_types_imported_header_shapes(self):
        server_mod = {
            "name": "rpc_server_module",
            "type": "rpc_server",
            "transport": "json",
            "service_name": "AddTwoInts",
            "ros2_import_json_types_header": "add_two_ints_json_types.hpp",
            "methods": [{
                "name": "AddTwoInts",
                "request": "migration_practice::AddTwoInts_Request",
                "response": "migration_practice::AddTwoInts_Response",
            }],
        }
        client_mod = dict(server_mod, name="rpc_client_module", type="rpc_client")

        server_ts = self.spec.emit_rpc_server_typesupport(
            server_mod, "migration_practice")
        client_ts = self.spec.emit_rpc_client_typesupport(
            client_mod, "migration_practice")

        self.assertIn('#include "add_two_ints_json_types.hpp"', server_ts["inc_h_content"])
        self.assertIn('#include "add_two_ints_json_types.hpp"', client_ts["inc_h_content"])
        self.assertNotIn('#include "json_custom_types.h"', server_ts["inc_h_content"])
        self.assertNotIn('#include "json_custom_types.h"', client_ts["inc_h_content"])
        self.assertIn(
            "RegisterServiceFunc<migration_practice::AddTwoInts_Request, migration_practice::AddTwoInts_Response>",
            server_ts["inc_cc_content"],
        )
        self.assertIn(
            "Call<migration_practice::AddTwoInts_Request, migration_practice::AddTwoInts_Response>",
            client_ts["inc_cc_content"],
        )
        self.assertIn("namespace migration_practice {", client_ts["inc_cc_content"])
        self.assertIn("kServiceName", client_ts["inc_cc_content"])

    def test_json_rpc_client_method_scoped_marker_contract(self):
        mod = {
            "name": "demo_json_rpc_client",
            "type": "rpc_client",
            "transport": "json",
            "service_name": "DemoService",
            "methods": [
                {
                    "name": "AddTwoInts",
                    "request": "demo::AddReq",
                    "response": "demo::AddRsp",
                },
                {
                    "name": "ResetCounter",
                    "request": "demo::ResetReq",
                    "response": "demo::ResetRsp",
                },
            ],
        }

        self.assertEqual(
            self.spec.rpc_client_markers,
            _canonical_rpc_client_markers(),
        )
        json_transport = import_module("transports.json")
        ts_info = json_transport._gen_json_rpc_client_markerized_typesupport(
            mod, "ibmw::demo"
        )
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]

        for method_name in ("AddTwoInts", "ResetCounter"):
            request_marker = _method_marker(self.spec.rpc_client_markers[0], method_name)
            response_marker = _method_marker(self.spec.rpc_client_markers[1], method_name)
            status_marker = _method_marker(self.spec.rpc_client_markers[2], method_name)
            call_anchor = (
                f'auto status = ibmw::json::rpc::Call<demo::{"AddReq" if method_name == "AddTwoInts" else "ResetReq"}, '
                f'demo::{"AddRsp" if method_name == "AddTwoInts" else "ResetRsp"}>('
            )
            for marker in (request_marker, response_marker, status_marker):
                begin = f"// {marker}_BEGIN"
                end = f"// {marker}_END"
                self.assertIn(begin, inc_cc)
                self.assertIn(end, inc_cc)
                self.assertLess(inc_cc.index(begin), inc_cc.index(end))
            self.assertIn(call_anchor, inc_cc)
            self.assertLess(
                inc_cc.index(f"// {request_marker}_END"),
                inc_cc.index(call_anchor),
            )

        self.assertIn("void RpcLoop();", inc_h)
        self.assertNotIn("co_await", inc_cc)
        self.assertIn("std::this_thread::sleep_for", inc_cc)

    def test_emit_returns_none_for_unknown_type(self):
        mod = {
            "name": "x",
            "type": "totally_made_up_type",
            "transport": "json",
        }
        self.assertIsNone(self.spec.emit_module(mod, "ns", "x_module"))


if __name__ == "__main__":
    _ = unittest.main()
