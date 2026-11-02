#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm plugin emit_module contract tests.
#
# Mirrors test_iceoryx_plugin_emit.py for the self-developed shm transport
# (shm_ext). Verifies:
#   1. shm plugin registered via transports.get("shm").
#   2. emit_module callable exists rather than a None stub.
#   3. Plugin `owns_type_keys` covers loaned_publisher + loaned_subscriber.
#   4. module_gen._GENERATORS no longer contains any (*, "shm") row.
#   5. Plugin-emitted bytes for a synthetic loaned_publisher dict are
#      shaped like real generated code.
#   6. emit_module returns None for unknown type (fall-through contract).
#   7. schema / cmake_gen / config_gen / ros2 import wiring of "shm"
#      (no iceoryx_posh, no RouDi, shm_extension auto-added).
#
# Usage:
#   python3 -m pytest tests/test_shm_plugin_emit.py -v

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))

EXPECTED_SHM_TYPE_KEYS = {
    "direct_shm_publisher",
    "direct_shm_subscriber",
    "loaned_publisher",
    "loaned_subscriber",
}


class TestShmPluginEmit(unittest.TestCase):
    def setUp(self):
        import transports
        from generators import module_gen

        self.transports = transports
        self.module_gen = module_gen
        self.spec = transports.get("shm")

    def test_shm_plugin_registered(self):
        self.assertIsNotNone(self.spec, "shm plugin not registered")
        self.assertEqual(self.spec.name, "shm")

    def test_emit_module_callable(self):
        self.assertIsNotNone(
            self.spec.emit_module,
            "shm plugin emit_module is not wired",
        )

    def test_owns_all_expected_shm_type_keys(self):
        owned = set(self.spec.meta.get("owns_type_keys", []))
        missing = EXPECTED_SHM_TYPE_KEYS - owned
        self.assertFalse(
            missing,
            f"plugin does not claim these shm types: {sorted(missing)}",
        )

    def test_module_gen_no_longer_has_shm_keys(self):
        # The legacy table must not contain any (*, "shm") entries
        # — those generators all live in transports/shm.py now.
        leftover = {
            (t, tr) for (t, tr) in self.module_gen._GENERATORS
            if tr == "shm"
        }
        self.assertFalse(
            leftover,
            f"module_gen._GENERATORS still has shm keys after plugin extraction: "
            f"{sorted(leftover)} — these must move to transports/shm.py",
        )

    def test_emit_loaned_publisher_shape_and_stability(self):
        # loaned_publisher flows through the unified
        # path via emit_loaned_publisher_typesupport. emit_module returns
        # None; the 4-file pair is materialised by module_gen's unified
        # emitter (see _gen_loaned_publisher_unified).
        mod = {
            "name": "demo_loan_pub",
            "type": "loaned_publisher",
            "transport": "shm",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/demo_loan",
            "shm_msg_type": "geometry_msgs/msg/Point",
            "zero_copy_config": "strict",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_loan_pub_module"

        # emit_module defers loaned_publisher to the unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_loaned_publisher_typesupport hook is wired.
        self.assertIsNotNone(
            getattr(self.spec, "emit_loaned_publisher_typesupport", None),
            "shm plugin emit_loaned_publisher_typesupport hook missing "
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
        self.assertNotIn("shm_ext/", inc_h)
        self.assertNotIn("iceoryx", inc_h)
        self.assertNotIn("ShmTransportDriver", inc_h)
        self.assertNotIn("SetShmDriver", inc_h)
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
        self.assertNotIn("shm_transport_driver.h", user_h)
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
            "transport": "shm",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/demo_loan",
            "shm_msg_type": "geometry_msgs/msg/Point",
            "subscriber_style": "callback",
        }
        ns = "ibmw::demo"
        mod_dir = "demo_loan_sub_module"

        # emit_module defers loaned_subscriber to the unified path.
        self.assertIsNone(self.spec.emit_module(mod, ns, mod_dir))

        # emit_loaned_subscriber_typesupport shape.
        self.assertIsNotNone(
            getattr(self.spec, "emit_loaned_subscriber_typesupport", None),
            "shm plugin emit_loaned_subscriber_typesupport hook missing "
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
        self.assertNotIn("iceoryx", inc_h)
        self.assertNotIn("iceoryx", inc_cc)

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
            "transport": "shm",
        }
        self.assertIsNone(self.spec.emit_module(mod, "ns", "x_module"))

    def test_direct_shm_publisher_emit_with_r1_anchor(self):
        mod = {
            "name": "direct_pub",
            "type": "direct_shm_publisher",
            "transport": "shm",
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
        include = header.index('#include "shm_ext/shm_transport_driver.h"')
        end = header.index("// IBMW_R1_EXEMPT_END")
        self.assertLess(begin, include)
        self.assertLess(include, end)
        self.assertIn("cpp/transport/transport_factory.h", header)
        # publisher_for<T>() needs the ROS2 type-support traits of T.
        self.assertIn('#include "type_support/ros2/ros2_transport.h"', header)
        self.assertIn("publisher_for<geometry_msgs::msg::Point>", source)

    def test_direct_shm_subscriber_emit_with_r1_anchor(self):
        mod = {
            "name": "direct_sub",
            "type": "direct_shm_subscriber",
            "transport": "shm",
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
        include = header.index('#include "shm_ext/shm_transport_driver.h"')
        end = header.index("// IBMW_R1_EXEMPT_END")
        self.assertLess(begin, include)
        self.assertLess(include, end)
        self.assertIn("cpp/transport/transport_factory.h", header)
        self.assertIn('#include "type_support/ros2/ros2_transport.h"', header)
        self.assertIn("subscriber_for<geometry_msgs::msg::Point>", source)

    def test_direct_shm_publisher_implicit_transport(self):
        import schema

        data = {
            "schema_version": 2,
            "_yaml_path": "<unit>",
            "messages": {
                "geometry_msgs::msg::Twist": {
                    "wire": {
                        "shm": {
                            "msg_type": "geometry_msgs/msg/Twist",
                        },
                    },
                },
            },
            "modules": [{
                "name": "dpub",
                "type": "direct_shm_publisher",
                "topic": "/twist",
                "message": "geometry_msgs::msg::Twist",
            }],
        }

        schema._expand_v2_to_v1_internal(data)
        schema._validate_modules(data)
        mod = data["modules"][0]

        self.assertEqual(mod.get("transport"), "shm")
        self.assertEqual(mod.get("message_type"), "geometry_msgs::msg::Twist")
        self.assertEqual(mod.get("shm_msg_type"), "geometry_msgs/msg/Twist")
        self.assertEqual(mod.get("ros2_msg_include"), "geometry_msgs/msg/twist.hpp")
        self.assertEqual(mod.get("ros2_package"), "geometry_msgs")
        self.assertEqual(mod.get("ros2_packages"), ["geometry_msgs"])

    def test_migrate_yaml_v1_to_v2_direct_shm(self):
        import migrate_yaml

        data = {
            "project": {"name": "demo"},
            "modules": [{
                "name": "dpub",
                "type": "direct_shm_publisher",
                "topic": "/twist",
                "message_type": "geometry_msgs::msg::Twist",
                "shm_msg_type": "geometry_msgs/msg/Twist",
            }],
        }

        out = migrate_yaml.migrate(data)
        mod = out["modules"][0]
        msg = out["messages"]["geometry_msgs::msg::Twist"]

        self.assertEqual(out["schema_version"], 2)
        self.assertEqual(mod["type"], "direct_shm_publisher")
        self.assertEqual(mod["topic"], "/twist")
        self.assertEqual(mod["message"], "geometry_msgs::msg::Twist")
        self.assertNotIn("message_type", mod)
        self.assertNotIn("transport", mod)
        self.assertEqual(msg["wire"]["shm"]["msg_type"], "geometry_msgs/msg/Twist")

    # ── Guard: shm_msg_type omission yields byte-identical output
    # when its value would equal message_type (the common case in every known
    # yaml). Guards against regressions in the `.get(..., message_type)`
    # fallback that preserves the byte-equal regen chain.
    def test_emit_byte_identical_when_shm_msg_type_omitted(self):
        mod_with = {
            "name": "pub_x",
            "type": "loaned_publisher",
            "transport": "shm",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/t",
            "shm_msg_type": "geometry_msgs::msg::Point",
            "zero_copy_config": "strict",
        }
        mod_without = dict(mod_with)
        del mod_without["shm_msg_type"]

        # loaned_publisher flows through the unified path; compare
        # via the dedicated typesupport hook (emit_module returns None).
        out_with = self.spec.emit_loaned_publisher_typesupport(mod_with, "ns")
        out_without = self.spec.emit_loaned_publisher_typesupport(
            mod_without, "ns")
        self.assertEqual(out_with, out_without)

        mod_sub_with = {
            "name": "sub_x",
            "type": "loaned_subscriber",
            "transport": "shm",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "topic": "/t",
            "shm_msg_type": "geometry_msgs::msg::Point",
            "subscriber_style": "callback",
        }
        mod_sub_without = dict(mod_sub_with)
        del mod_sub_without["shm_msg_type"]

        out_sub_with = self.spec.emit_module(
            mod_sub_with, "ns", "sub_x_module")
        out_sub_without = self.spec.emit_module(
            mod_sub_without, "ns", "sub_x_module")
        self.assertEqual(out_sub_with, out_sub_without)


# Cross-feature regression test that
# exercises the full ibmw-gen CLI pipeline end-to-end on a synthetic
# direct_shm_* project. Guards against regressions where individual
# emitter unit-tests (above) pass but the assembled CLI run drops the
# IBMW_R1 anchor, fails to link shm_ext from the generated CMake, or
# loses the implicit `transport: shm` derivation in the generated cfg
# yaml. Spawns the real ibmw_gen.py via subprocess (mirrors the harness
# in test_no_overwrite.py) — must NOT bypass the CLI.

import json
import os
import shutil
import subprocess
import tempfile

IBMW_GEN_CLI = ROOT / "ibmw_gen.py"

SYNTHETIC_DIRECT_SHM_YAML = """\
# Auto-generated synthetic fixture for the regression test.
# Do not edit; constructed inline by test_full_pipeline_direct_shm_*.
schema_version: 2
project:
  name: r8_pipeline
  namespace: ibmw::tests::r8_pipeline

messages:
  geometry_msgs::msg::Point:
    wire:
      shm:
        msg_type: geometry_msgs/msg/Point

modules:
  - name: DirectPub
    type: direct_shm_publisher
    topic: /r8_pipeline/point
    message: geometry_msgs::msg::Point
  - name: DirectSub
    type: direct_shm_subscriber
    topic: /r8_pipeline/point
    message: geometry_msgs::msg::Point

apps:
  - name: r8_pipeline_app
    type: pubsub
    modules:
      - DirectPub
      - DirectSub
"""


class TestFullPipelineDirectShm(unittest.TestCase):
    """End-to-end CLI regression for direct_shm_* generation.

    Guards three cross-step invariants that no single emitter-level test
    would catch on its own:

      (1) The IBMW_R1_EXEMPT anchor wraps the shm_ext include in
          every emitted module .h (Step A's emitter contract survives the
          full driver+app+cfg pipeline).
      (2) The generated CMakeLists.txt links `shm_ext` (Step F's
          cmake_gen wiring survives schema v2 → v1 expansion).
      (3) The generated app cfg yaml carries `transport: shm` (the
          implicit-transport derivation in schema._expand_v2_to_v1_internal
          survives the CLI's full expand → emit → write chain).
    """

    def setUp(self):
        self.tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_r8_"))
        self.yaml_path = self.tmpdir / "r8_pipeline.ibmw.yaml"
        self.out_dir = self.tmpdir / "r8_pipeline_gen"
        self.yaml_path.write_text(SYNTHETIC_DIRECT_SHM_YAML)

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

    def test_full_pipeline_direct_shm_emits_r1_anchor_and_shm_link(self):
        rc = self._run_cli()
        self.assertEqual(
            rc.returncode, 0,
            f"ibmw_gen CLI failed (rc={rc.returncode})\n"
            f"stdout:\n{rc.stdout}\nstderr:\n{rc.stderr}")

        # (1) IBMW_R1_EXEMPT anchor wraps shm_ext include in every
        # generated direct_shm_* module header. Module dirs follow
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
                '#include "shm_ext/shm_transport_driver.h"',
                header,
                f"{mod_dir} header missing shm_ext include")
            self.assertIn("// IBMW_R1_EXEMPT_END", header,
                          f"{mod_dir} header missing R1 anchor end")
            begin = header.index("// IBMW_R1_EXEMPT_BEGIN")
            include = header.index(
                '#include "shm_ext/shm_transport_driver.h"')
            end = header.index("// IBMW_R1_EXEMPT_END")
            self.assertLess(
                begin, include,
                f"{mod_dir} header: R1 begin anchor not before include")
            self.assertLess(
                include, end,
                f"{mod_dir} header: R1 end anchor not after include")

        # (2) Top-level CMakeLists.txt links shm_ext, which needs no
        # iceoryx_posh (unlike iceoryx_ext).
        cml = (self.out_dir / "CMakeLists.txt").read_text()
        self.assertIn(
            "ibmw::extensions::shm_ext", cml,
            "Top-level CMakeLists.txt does not reference shm_ext "
            "— Step F cmake_gen wiring regression")
        self.assertNotIn("iceoryx_posh", cml)
        self.assertNotIn("iceoryx_ext", cml)
        self.assertIn(
            "ibmw_whole_archive_link(direct_pub ibmw_extensions_ros2_types_impl)",
            cml)

        # (3) Generated app cfg yaml carries implicit `transport: shm`.
        cfg_dir = self.out_dir / "cfg"
        self.assertTrue(
            cfg_dir.exists(),
            f"cfg/ directory missing under {self.out_dir}")
        app_cfgs = sorted(cfg_dir.glob("*.yaml"))
        self.assertTrue(
            app_cfgs,
            f"no generated app cfg yaml found under {cfg_dir}")
        # The generated cfgs (app-mode and pkg-mode) must load the shm
        # extension — the v2→v1 expansion must have promoted the module
        # to `transport: shm`.
        joined = "\n".join(p.read_text() for p in app_cfgs)
        self.assertIn(
            "name: shm_extension", joined,
            "no generated cfg yaml loads shm_extension — implicit-transport "
            "derivation regression in schema._expand_v2_to_v1_internal")
        # There is no RouDi prerequisite.
        self.assertIn("libibmw_shm_transport.so", joined)
        self.assertNotIn("RouDi", joined)
        self.assertNotIn("iceoryx", joined)

        # (4) publisher_for<T>() / subscriber_for<T>() route through the
        # messaging layer: without the shm driver and the topic in the
        # messaging block nothing is sent or received.
        for cfg in app_cfgs:
            text = cfg.read_text()
            msg = text[text.index("  messaging:"):]
            self.assertIn("- type: shm", msg, cfg.name)
            for section in ("publishers:", "subscribers:"):
                block = msg[msg.index(section):]
                self.assertIn('- topic: "/r8_pipeline/point"', block, f"{cfg.name} {section}")
                self.assertIn("enable_drivers: [shm]", block, f"{cfg.name} {section}")


SYNTHETIC_LOANED_SHM_YAML = """\
schema_version: 2
project:
  name: shm_loaned_pipeline
  namespace: ibmw::tests::shm_loaned_pipeline

messages:
  geometry_msgs::msg::Point:
    wire:
      shm:
        msg_type: geometry_msgs/msg/Point

modules:
  - name: LoanPub
    type: loaned_publisher
    transport: shm
    topic: /shm_loaned/point
    message: geometry_msgs::msg::Point
  - name: LoanSub
    type: loaned_subscriber
    transport: shm
    topic: /shm_loaned/point
    message: geometry_msgs::msg::Point

apps:
  - name: shm_loaned_app
    type: pubsub
    modules:
      - LoanPub
      - LoanSub
"""


class TestFullPipelineLoanedShm(unittest.TestCase):
    """End-to-end CLI run for loaned_{publisher,subscriber} on shm: public
    loan API only, shm driver declared in the messaging block."""

    def setUp(self):
        self.tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="ibmw_gen_shm_"))
        self.yaml_path = self.tmpdir / "shm_loaned.ibmw.yaml"
        self.out_dir = self.tmpdir / "shm_loaned_gen"
        self.yaml_path.write_text(SYNTHETIC_LOANED_SHM_YAML)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_full_pipeline_loaned_shm_declares_shm_driver(self):
        rc = subprocess.run(
            [sys.executable, str(IBMW_GEN_CLI), str(self.yaml_path),
             "-o", str(self.out_dir)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(
            rc.returncode, 0,
            f"ibmw_gen CLI failed (rc={rc.returncode})\n"
            f"stdout:\n{rc.stdout}\nstderr:\n{rc.stderr}")
        self.assertNotIn("R8.2-error", rc.stderr)

        cml = (self.out_dir / "CMakeLists.txt").read_text()
        self.assertIn("ibmw::api::ros2_type_support", cml)
        self.assertNotIn("shm_ext", cml)
        self.assertNotIn("iceoryx_posh", cml)

        for mod_dir, name in (("loan_pub_module", "loan_pub"),
                              ("loan_sub_module", "loan_sub")):
            user_h = (self.out_dir / "src" / mod_dir / f"{name}.h").read_text()
            self.assertNotIn("shm_ext/", user_h)
            self.assertIn(f"{name}_typesupport.inc.h", user_h)

        joined = "\n".join(
            p.read_text() for p in sorted((self.out_dir / "cfg").glob("*.yaml")))
        self.assertIn("- type: shm", joined)
        self.assertIn("name: shm_extension", joined)
        self.assertNotIn("RouDi", joined)
        self.assertNotIn("iceoryx", joined)


class TestShmSchemaAndImportWiring(unittest.TestCase):
    """Schema, config_gen and ros2 interface import accept "shm" like iceoryx."""

    def _loaned(self, mod_type: str, **extra) -> dict:
        import schema

        mod = {
            "name": "m",
            "type": mod_type,
            "transport": "shm",
            "topic": "/p",
            "message_type": "geometry_msgs::msg::Point",
        }
        mod.update(extra)
        data = {"_yaml_path": "<unit>", "modules": [mod]}
        schema._validate_modules(data)
        return data["modules"][0]

    def test_loaned_publisher_and_subscriber_accept_shm(self):
        import contextlib
        import io

        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            pub = self._loaned("loaned_publisher")
            sub = self._loaned("loaned_subscriber")
        # shm is SHM-capable: no R8.2 diagnostic.
        self.assertNotIn("R8.2-error", err.getvalue())
        self.assertEqual(pub["transport"], "shm")
        self.assertEqual(pub["zero_copy_config"], "strict")
        self.assertTrue(pub["_is_loaned_publisher"])
        self.assertEqual(pub["ros2_packages"], ["geometry_msgs"])
        self.assertEqual(sub["transport"], "shm")
        self.assertTrue(sub["_is_loaned_subscriber"])

    def test_loaned_publisher_shm_rejects_non_strict(self):
        with self.assertRaises(SystemExit):
            self._loaned("loaned_publisher", zero_copy_config="auto")

    def test_direct_shm_forces_shm_transport(self):
        mod = self._loaned("direct_shm_subscriber", transport="dds")
        self.assertEqual(mod["transport"], "shm")
        self.assertTrue(mod["_is_direct_shm"])
        self.assertNotIn("_is_direct_iceoryx", mod)

    def test_config_gen_shm_driver_and_extension_options(self):
        from generators import config_gen

        module_map = {"m": {"name": "m", "type": "loaned_publisher",
                            "transport": "shm", "topic": "/p"}}
        self.assertEqual(config_gen._get_messaging_driver(["m"], module_map),
                         "shm")
        project = {
            "project": {"_namespace_cpp": "ns"},
            "shm_driver": {"extension_options": {"shm_init_size": 4096,
                                                 "domain": "demo"}},
        }
        text = "\n".join(config_gen._gen_extensions_section(
            project, ["m"], module_map))
        self.assertIn("name: shm_extension", text)
        self.assertIn("libibmw_shm_transport.so", text)
        self.assertIn("shm_init_size: 4096", text)
        self.assertIn('domain: "demo"', text)
        self.assertNotIn("iceoryx", text)

    def test_config_gen_direct_shm_topics_reach_messaging(self):
        from generators import config_gen

        module_map = {
            "p": {"name": "p", "type": "direct_shm_publisher", "transport": "shm", "topic": "/t"},
            "s": {"name": "s", "type": "direct_shm_subscriber", "transport": "shm", "topic": "/t"},
        }
        self.assertTrue(config_gen._needs_messaging(["p"], module_map))
        self.assertEqual(config_gen._collect_topics(["p", "s"], module_map),
                         {"/t": {"publisher", "subscriber"}})

    def test_config_gen_rejects_iceoryx_and_shm_in_one_app(self):
        from generators import config_gen

        module_map = {
            "ipub": {"name": "ipub", "type": "loaned_publisher", "transport": "iceoryx", "topic": "/a"},
            "ssub": {"name": "ssub", "type": "direct_shm_subscriber", "transport": "shm", "topic": "/b"},
            "dsub": {"name": "dsub", "type": "subscriber", "transport": "dds", "topic": "/c"},
        }
        with self.assertRaises(ValueError) as ctx:
            config_gen._get_messaging_driver(["ipub", "ssub", "dsub"], module_map)
        message = str(ctx.exception)
        self.assertIn("cannot mix iceoryx and shm", message)
        self.assertIn("iceoryx: ipub", message)
        self.assertIn("shm: ssub", message)
        # Either one alone (with other transports) is still fine.
        self.assertEqual(config_gen._get_messaging_driver(["ipub", "dsub"], module_map), "iceoryx")
        self.assertEqual(config_gen._get_messaging_driver(["ssub", "dsub"], module_map), "shm")

    def test_schema_rejects_iceoryx_and_shm_in_one_app(self):
        import contextlib
        import io

        import schema

        data = {
            "modules": [
                {"name": "ipub", "transport": "iceoryx"},
                {"name": "ssub", "transport": "shm"},
                {"name": "dsub", "transport": "dds"},
            ],
            "apps": [{"name": "ok_app", "modules": ["ssub", "dsub"]},
                     {"name": "mixed_app", "modules": ["ipub", "ssub"]}],
        }
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as ctx:
            schema._validate_apps(data)
        self.assertEqual(ctx.exception.code, 1)
        self.assertIn("App 'mixed_app' cannot mix iceoryx and shm modules (iceoryx: ipub; shm: ssub)", err.getvalue())
        data["apps"] = data["apps"][:1]
        schema._validate_apps(data)  # shm with other transports is fine

    def test_ros2_import_shm_target_metadata(self):
        from ros2_interface_import import emit_reference_existing_interface_report

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            pkg = root / "ros" / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>existing_interfaces</name>"
                "<version>0.1.0</version></package>\n", encoding="utf-8")
            pkg.joinpath("msg", "Plain.msg").write_text(
                "float64 x\nfloat64 y\n", encoding="utf-8")
            pkg.joinpath("msg", "Dynamic.msg").write_text(
                "string label\n", encoding="utf-8")
            out = root / "out"

            report = emit_reference_existing_interface_report(
                str(root / "ros"), "existing_interfaces", str(out),
                targets=["dds_ros2", "shm"])

            self.assertEqual(report["target_backend_status"]["summary"],
                             {"dds_ros2": "supported", "shm": "degraded"})
            self.assertEqual(report["backend_metadata_files"],
                             ["existing_interfaces_shm_backend_metadata.json"])
            status = {e["target"]: e for e in
                      report["target_backend_status"]["entries"]}
            self.assertEqual(status["shm"]["runtime_boundary"],
                             "metadata_only_no_shm_lowering")
            backend = json.loads(out.joinpath(
                "existing_interfaces_shm_backend_metadata.json").read_text(
                    encoding="utf-8"))
            self.assertEqual(backend["target"], "shm")
            self.assertIn("existing_interfaces/msg/Plain",
                          backend["eligible_interfaces"])
            self.assertIn("existing_interfaces/msg/Dynamic",
                          backend["blocked_interfaces"])
            plan = json.loads(out.joinpath(
                "existing_interfaces_shm_lowering_plan.json").read_text(
                    encoding="utf-8"))
            self.assertEqual(plan["artifact"], "shm_lowering_plan")
            self.assertEqual(plan["totals"], {"eligible": 1, "blocked": 1})
            self.assertEqual(
                plan["eligible_interfaces"][0]["future_type_name"],
                "existing_interfaces::PlainShmType")

    def test_ibmw_gen_cli_accepts_shm_import_target(self):
        rc = subprocess.run(
            [sys.executable, str(IBMW_GEN_CLI), "import", "--help"],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(rc.returncode, 0, rc.stderr)
        self.assertIn("{dds_ros2,dds_native,iceoryx,shm,json}", rc.stdout)


if __name__ == "__main__":
    unittest.main()
