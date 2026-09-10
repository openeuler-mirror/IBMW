#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Iceoryx plugin emit_module contract tests.
#
# Mirrors test_dds_plugin_emit.py. Verifies:
#   1. iceoryx plugin registered via transports.get("iceoryx").
#   2. emit_module callable exists rather than a None stub.
#   3. Plugin `owns_type_keys` covers loaned_publisher + loaned_subscriber.
#   4. module_gen._GENERATORS no longer contains any (*, "iceoryx") row.
#   5. Plugin-emitted bytes for a synthetic loaned_publisher dict are
#      shaped like real generated code.
#   6. emit_module returns None for unknown type (fall-through contract).
#
# Usage:
#   python3 -m pytest tests/test_iceoryx_plugin_emit.py -v

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))

EXPECTED_ICEORYX_TYPE_KEYS = {
    "direct_iceoryx_publisher",
    "direct_iceoryx_subscriber",
    "loaned_publisher",
    "loaned_subscriber",
}


class TestIceoryxPluginEmit(unittest.TestCase):
    def setUp(self):
        import transports
        from generators import module_gen

        self.transports = transports
        self.module_gen = module_gen
        self.spec = transports.get("iceoryx")

    def test_iceoryx_plugin_registered(self):
        self.assertIsNotNone(self.spec, "iceoryx plugin not registered")
        self.assertEqual(self.spec.name, "iceoryx")

    def test_emit_module_callable(self):
        self.assertIsNotNone(
            self.spec.emit_module,
            "iceoryx plugin emit_module is not wired",
        )

    def test_owns_all_expected_iceoryx_type_keys(self):
        owned = set(self.spec.meta.get("owns_type_keys", []))
        missing = EXPECTED_ICEORYX_TYPE_KEYS - owned
        self.assertFalse(
            missing,
            f"plugin does not claim these iceoryx types: {sorted(missing)}",
        )

    def test_module_gen_no_longer_has_iceoryx_keys(self):
        # The legacy table must not contain any (*, "iceoryx") entries
        # — those generators all live in transports/iceoryx.py now.
        leftover = {
            (t, tr) for (t, tr) in self.module_gen._GENERATORS
            if tr == "iceoryx"
        }
        self.assertFalse(
            leftover,
            f"module_gen._GENERATORS still has iceoryx keys after plugin extraction: "
            f"{sorted(leftover)} — these must move to transports/iceoryx.py",
        )

    def test_emit_loaned_publisher_shape_and_stability(self):
        # loaned_publisher flows through the unified
        # path via emit_loaned_publisher_typesupport. emit_module returns
        # None; the 4-file pair is materialised by module_gen's unified
        # emitter (see _gen_loaned_publisher_unified).
        mod = {
            "name": "demo_loan_pub",
            "type": "loaned_publisher",
            "transport": "iceoryx",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/demo_loan",
            "iceoryx_msg_type": "geometry_msgs/msg/Point",
            "zero_copy_config": "strict",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_loan_pub_module"

        # emit_module defers loaned_publisher to the unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_loaned_publisher_typesupport hook is wired.
        self.assertIsNotNone(
            getattr(self.spec, "emit_loaned_publisher_typesupport", None),
            "iceoryx plugin emit_loaned_publisher_typesupport hook missing "
            "— unified hook not wired",
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
        self.assertIn("std::is_trivially_copyable_v", inc_h)
        # Seven-macro public loan contract.
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
        self.assertIn("RegisterLoanedPodTopic", inc_h)
        self.assertIn("publisher_->Loan()", inc_h)
        self.assertIn("publisher_->Publish", inc_h)
        self.assertNotIn("iceoryx_ext/", inc_h)
        self.assertNotIn("IceoryxTransportDriver", inc_h)
        self.assertNotIn("SetIceoryxDriver", inc_h)
        self.assertNotIn("SetDriverSlot(", inc_h)
        self.assertNotIn("EnsurePodTopic", inc_h)
        # Hot-loop user-code marker pair lives inside the macro.
        self.assertIn("populate_loaned_message", inc_h)

        # Unified end-to-end: 4 files + R1/R2/R3/R9 guards.
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
        self.assertNotIn("cpp/transport/loaned_sample.h", user_h)
        self.assertNotIn("iceoryx_transport_driver.h", user_h)
        self.assertNotIn("ros2_types/transport/ros2_transport.h", user_h)
        self.assertNotIn("geometry_msgs/msg/point.hpp", user_h)
        self.assertIn("demo_loan_pub_typesupport.inc.h", user_h)
        # R2: no concrete LoanedSample<T> / Borrow / Publish in user .cc.
        self.assertNotIn(
            "LoanedSample<geometry_msgs::msg::Point>", user_cc)
        self.assertNotIn("EnsurePodTopic", user_cc)
        self.assertIn("IBMW_TS_LOANED_PUB_HOT_LOOP_BODY", user_cc)
        self.assertIn("IBMW_TS_LOANED_PUB_SETUP", user_cc)
        self.assertIn("IBMW_TS_LOANED_PUB_RUN_INIT", user_cc)
        # R9: user.cc includes the inc.cc TU fragment.
        self.assertIn("demo_loan_pub_typesupport.inc.cc", user_cc)

    def test_emit_loaned_subscriber_shape(self):
        # loaned_subscriber flows through the unified
        # path via emit_loaned_subscriber_typesupport. emit_module returns
        # None; the 4-file pair is materialised by module_gen's unified
        # emitter.
        mod = {
            "name": "demo_loan_sub",
            "type": "loaned_subscriber",
            "transport": "iceoryx",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/demo_loan",
            "iceoryx_msg_type": "geometry_msgs/msg/Point",
            "subscriber_style": "callback",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_loan_sub_module"

        # emit_module defers loaned_subscriber to the unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_loaned_subscriber_typesupport shape.
        self.assertIsNotNone(
            getattr(self.spec, "emit_loaned_subscriber_typesupport", None),
            "iceoryx plugin emit_loaned_subscriber_typesupport hook missing "
            "— unified hook not wired",
        )
        ts_info = self.spec.emit_loaned_subscriber_typesupport(mod, ns)
        self.assertIsNotNone(ts_info)
        self.assertEqual(ts_info["msg_type_cpp"], "geometry_msgs::msg::Point")
        inc_h = ts_info["inc_h_content"]
        inc_cc = ts_info["inc_cc_content"]
        # inc.h carries the transport-specific includes and macro contract.
        self.assertIn("cpp/transport/loaned_subscriber.h", inc_h)
        self.assertIn("type_support/ros2/ros2_transport.h", inc_h)
        self.assertIn("geometry_msgs/msg/point.hpp", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_MSG_TYPE", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_STATIC_ASSERT", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL", inc_h)
        self.assertIn("IBMW_TS_LOANED_SUB_ATTACH", inc_h)
        self.assertIn("std::is_trivially_copyable_v", inc_h)
        self.assertIn("LoanedSubscriber<", inc_h)
        self.assertIn("AttachBytes(", inc_h)
        # inc.cc carries the default OnMessage body with the user-code marker.
        self.assertIn("on_message_body", inc_cc)
        self.assertIn("OnMessage(const geometry_msgs::msg::Point&", inc_cc)

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
        # R1: no transport-layer headers in the user-facing .h.
        self.assertNotIn("cpp/transport/loaned_subscriber.h", user_h)
        self.assertNotIn("ros2_types/transport/ros2_transport.h", user_h)
        self.assertNotIn("geometry_msgs/msg/point.hpp", user_h)
        self.assertIn("demo_loan_sub_typesupport.inc.h", user_h)
        # R2: no LoanedSubscriber<ConcreteType> in main .cc (it lives in
        # the macro expansion inside inc.cc only).
        self.assertNotIn(
            "LoanedSubscriber<geometry_msgs::msg::Point>", user_cc)
        self.assertIn("IBMW_TS_LOANED_SUB_ATTACH", user_cc)
        # R3: user.cc includes the transport-specific inc.cc TU fragment.
        self.assertIn("demo_loan_sub_typesupport.inc.cc", user_cc)

    def test_emit_returns_none_for_unknown_type(self):
        mod = {
            "name": "x",
            "type": "totally_made_up_type",
            "transport": "iceoryx",
        }
        self.assertIsNone(self.spec.emit_module(mod, "ns", "x_module"))

    def test_direct_iceoryx_publisher_emit_with_r1_anchor(self):
        mod = {
            "name": "direct_pub",
            "type": "direct_iceoryx_publisher",
            "transport": "iceoryx",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/direct_pub",
        }

        out = self.spec.emit_module(mod, "ibmw::demo", "direct_pub_module")
        self.assertIsNotNone(out)
        out_by_path = dict(out)
        header = out_by_path["src/direct_pub_module/direct_pub.h"]
        source = out_by_path["src/direct_pub_module/direct_pub.cc"]

        begin = header.index("// IBMW_R1_EXEMPT_BEGIN")
        include = header.index('#include "iceoryx_ext/iceoryx_transport_driver.h"')
        end = header.index("// IBMW_R1_EXEMPT_END")
        self.assertLess(begin, include)
        self.assertLess(include, end)
        self.assertIn("cpp/transport/transport_factory.h", header)
        self.assertIn("publisher_for<geometry_msgs::msg::Point>", source)

    def test_direct_iceoryx_subscriber_emit_with_r1_anchor(self):
        mod = {
            "name": "direct_sub",
            "type": "direct_iceoryx_subscriber",
            "transport": "iceoryx",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/direct_sub",
        }

        out = self.spec.emit_module(mod, "ibmw::demo", "direct_sub_module")
        self.assertIsNotNone(out)
        out_by_path = dict(out)
        header = out_by_path["src/direct_sub_module/direct_sub.h"]
        source = out_by_path["src/direct_sub_module/direct_sub.cc"]

        begin = header.index("// IBMW_R1_EXEMPT_BEGIN")
        include = header.index('#include "iceoryx_ext/iceoryx_transport_driver.h"')
        end = header.index("// IBMW_R1_EXEMPT_END")
        self.assertLess(begin, include)
        self.assertLess(include, end)
        self.assertIn("cpp/transport/transport_factory.h", header)
        self.assertIn("subscriber_for<geometry_msgs::msg::Point>", source)

    def test_direct_iceoryx_publisher_implicit_transport(self):
        import schema

        data = {
            "schema_version": 2,
            "_yaml_path": "<unit>",
            "messages": {
                "geometry_msgs::msg::Twist": {
                    "wire": {
                        "iceoryx": {
                            "msg_type": "geometry_msgs/msg/Twist",
                        },
                    },
                },
            },
            "modules": [{
                "name": "dpub",
                "type": "direct_iceoryx_publisher",
                "topic": "/twist",
                "message": "geometry_msgs::msg::Twist",
            }],
        }

        schema._expand_v2_to_v1_internal(data)
        schema._validate_modules(data)
        mod = data["modules"][0]

        self.assertEqual(mod.get("transport"), "iceoryx")
        self.assertEqual(mod.get("message_type"), "geometry_msgs::msg::Twist")
        self.assertEqual(mod.get("iceoryx_msg_type"), "geometry_msgs/msg/Twist")
        self.assertEqual(mod.get("ros2_msg_include"), "geometry_msgs/msg/twist.hpp")
        self.assertEqual(mod.get("ros2_package"), "geometry_msgs")
        self.assertEqual(mod.get("ros2_packages"), ["geometry_msgs"])

    def test_migrate_yaml_v1_to_v2_direct_iceoryx(self):
        import migrate_yaml

        data = {
            "project": {"name": "demo"},
            "modules": [{
                "name": "dpub",
                "type": "direct_iceoryx_publisher",
                "topic": "/twist",
                "message_type": "geometry_msgs::msg::Twist",
                "iceoryx_msg_type": "geometry_msgs/msg/Twist",
            }],
        }

        out = migrate_yaml.migrate(data)
        mod = out["modules"][0]
        msg = out["messages"]["geometry_msgs::msg::Twist"]

        self.assertEqual(out["schema_version"], 2)
        self.assertEqual(mod["type"], "direct_iceoryx_publisher")
        self.assertEqual(mod["topic"], "/twist")
        self.assertEqual(mod["message"], "geometry_msgs::msg::Twist")
        self.assertNotIn("message_type", mod)
        self.assertNotIn("transport", mod)
        self.assertEqual(msg["wire"]["iceoryx"]["msg_type"], "geometry_msgs/msg/Twist")

    # ── Guard: iceoryx_msg_type omission yields byte-identical output
    # when its value would equal message_type (the common case in every known
    # yaml). Guards against regressions in the `.get(..., message_type)`
    # fallback that preserves the byte-equal regen chain.
    def test_emit_byte_identical_when_iceoryx_msg_type_omitted(self):
        mod_with = {
            "name": "pub_x",
            "type": "loaned_publisher",
            "transport": "iceoryx",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/t",
            "iceoryx_msg_type": "geometry_msgs::msg::Point",
            "zero_copy_config": "strict",
        }
        mod_without = dict(mod_with)
        del mod_without["iceoryx_msg_type"]

        # loaned_publisher flows through the unified path; compare
        # via the dedicated typesupport hook (emit_module returns None).
        out_with = self.spec.emit_loaned_publisher_typesupport(mod_with, "ns")
        out_without = self.spec.emit_loaned_publisher_typesupport(
            mod_without, "ns")
        self.assertEqual(out_with, out_without)

        mod_sub_with = {
            "name": "sub_x",
            "type": "loaned_subscriber",
            "transport": "iceoryx",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/t",
            "iceoryx_msg_type": "geometry_msgs::msg::Point",
            "subscriber_style": "callback",
        }
        mod_sub_without = dict(mod_sub_with)
        del mod_sub_without["iceoryx_msg_type"]

        out_sub_with = self.spec.emit_module(
            mod_sub_with, "ns", "sub_x_module")
        out_sub_without = self.spec.emit_module(
            mod_sub_without, "ns", "sub_x_module")
        self.assertEqual(out_sub_with, out_sub_without)


# Cross-feature regression test that
# exercises the full ibmw-gen CLI pipeline end-to-end on a synthetic
# direct_iceoryx_* project. Guards against regressions where individual
# emitter unit-tests (above) pass but the assembled CLI run drops the
# IBMW_R1 anchor, fails to link iceoryx_ext from the generated CMake, or
# loses the implicit `transport: iceoryx` derivation in the generated cfg
# yaml. Spawns the real ibmw_gen.py via subprocess (mirrors the harness
# in test_no_overwrite.py) — must NOT bypass the CLI.

import os
import shutil
import subprocess
import tempfile

IBMW_GEN_CLI = ROOT / "ibmw_gen.py"

SYNTHETIC_DIRECT_ICEORYX_YAML = """\
# Auto-generated synthetic fixture for the regression test.
# Do not edit; constructed inline by test_full_pipeline_direct_iceoryx_*.
schema_version: 2
project:
  name: r8_pipeline
  namespace: ibmw::tests::r8_pipeline

messages:
  geometry_msgs::msg::Point:
    wire:
      iceoryx:
        msg_type: geometry_msgs/msg/Point

modules:
  - name: DirectPub
    type: direct_iceoryx_publisher
    topic: /r8_pipeline/point
    message: geometry_msgs::msg::Point
  - name: DirectSub
    type: direct_iceoryx_subscriber
    topic: /r8_pipeline/point
    message: geometry_msgs::msg::Point

apps:
  - name: r8_pipeline_app
    type: pubsub
    modules:
      - DirectPub
      - DirectSub
"""


class TestFullPipelineDirectIceoryx(unittest.TestCase):
    """End-to-end CLI regression for direct_iceoryx_* generation.

    Guards three cross-step invariants that no single emitter-level test
    would catch on its own:

      (1) The IBMW_R1_EXEMPT anchor wraps the iceoryx_ext include in
          every emitted module .h (Step A's emitter contract survives the
          full driver+app+cfg pipeline).
      (2) The generated CMakeLists.txt links `iceoryx_ext` (Step F's
          cmake_gen wiring survives schema v2 → v1 expansion).
      (3) The generated app cfg yaml carries `transport: iceoryx` (the
          implicit-transport derivation in schema._expand_v2_to_v1_internal
          survives the CLI's full expand → emit → write chain).
    """

    def setUp(self):
        self.tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_r8_"))
        self.yaml_path = self.tmpdir / "r8_pipeline.ibmw.yaml"
        self.out_dir = self.tmpdir / "r8_pipeline_gen"
        self.yaml_path.write_text(SYNTHETIC_DIRECT_ICEORYX_YAML)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _run_cli(self):
        cmd = [
            sys.executable, str(IBMW_GEN_CLI),
            str(self.yaml_path),
            "-o", str(self.out_dir),
        ]
        return subprocess.run(
            cmd, capture_output=True, text=True, timeout=30)

    def test_full_pipeline_direct_iceoryx_emits_r1_anchor_and_iceoryx_link(self):
        rc = self._run_cli()
        self.assertEqual(
            rc.returncode, 0,
            f"ibmw_gen CLI failed (rc={rc.returncode})\n"
            f"stdout:\n{rc.stdout}\nstderr:\n{rc.stderr}")

        # (1) IBMW_R1_EXEMPT anchor wraps iceoryx_ext include in every
        # generated direct_iceoryx_* module header. Module dirs follow
        # ibmw-gen's snake_case-with-_module-suffix convention.
        for mod_dir in ("direct_pub_module", "direct_sub_module"):
            header_candidates = list(
                (self.out_dir / "src" / mod_dir).glob("*.h"))
            self.assertEqual(
                len(header_candidates), 1,
                f"expected exactly one header in src/{mod_dir}/, "
                f"got {header_candidates}")
            header = header_candidates[0].read_text()
            self.assertIn("// IBMW_R1_EXEMPT_BEGIN", header,
                          f"{mod_dir}/{header_candidates[0].name} missing R1 anchor begin")
            self.assertIn(
                '#include "iceoryx_ext/iceoryx_transport_driver.h"',
                header,
                f"{mod_dir} header missing iceoryx_ext include")
            self.assertIn("// IBMW_R1_EXEMPT_END", header,
                          f"{mod_dir} header missing R1 anchor end")
            begin = header.index("// IBMW_R1_EXEMPT_BEGIN")
            include = header.index(
                '#include "iceoryx_ext/iceoryx_transport_driver.h"')
            end = header.index("// IBMW_R1_EXEMPT_END")
            self.assertLess(
                begin, include,
                f"{mod_dir} header: R1 begin anchor not before include")
            self.assertLess(
                include, end,
                f"{mod_dir} header: R1 end anchor not after include")

        # (2) Top-level CMakeLists.txt links iceoryx_ext.
        cml = (self.out_dir / "CMakeLists.txt").read_text()
        self.assertIn(
            "iceoryx_ext", cml,
            "Top-level CMakeLists.txt does not reference iceoryx_ext "
            "— Step F cmake_gen wiring regression")

        # (3) Generated app cfg yaml carries implicit `transport: iceoryx`.
        cfg_dir = self.out_dir / "cfg"
        self.assertTrue(
            cfg_dir.exists(),
            f"cfg/ directory missing under {self.out_dir}")
        app_cfgs = sorted(cfg_dir.glob("*.yaml"))
        self.assertTrue(
            app_cfgs,
            f"no generated app cfg yaml found under {cfg_dir}")
        # At least one of the generated cfgs (app-mode or pkg-mode) must
        # carry the iceoryx transport driver — the v2→v1 expansion must
        # have promoted `wire.iceoryx` to a `transport: iceoryx` driver
        # entry somewhere in the messaging block.
        joined = "\n".join(p.read_text() for p in app_cfgs)
        self.assertIn(
            "iceoryx", joined,
            "no generated cfg yaml mentions iceoryx — implicit-transport "
            "derivation regression in schema._expand_v2_to_v1_internal")


if __name__ == "__main__":
    unittest.main()
