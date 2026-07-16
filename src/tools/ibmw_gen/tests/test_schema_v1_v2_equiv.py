#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Schema v1 / v2 equivalence fixture tests.
#
# Locks in the dual-track schema invariant between the v2 messages block and
# the v2→v1 in-memory expander:
#
#   For every transport (dds / ros2 / iceoryx) and the implicit-ros2
#   templates (direct_ros2_* / ros2_interop_*), a v1 "flat" yaml and
#   the equivalent v2 "messages-block" yaml MUST yield identical
#   module dicts after _expand_v2_to_v1_internal().
#
# Also covers:
#   * messages-block missing → no-op (v1 pass-through)
#   * unknown message ref → schema error
#   * schema_version validation (non-int / unsupported)
#
# Usage:
#   python3 -m pytest tests/test_schema_v1_v2_equiv.py -v

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))


def _expand(data):
    """Apply the v2→v1 expander to a yaml-equivalent dict (in-place)."""
    import schema
    schema._expand_v2_to_v1_internal(data)
    return data


def _module_subset(mod, keys):
    """Return only the keys we care about — drops generator-injected
    `_*` fields and id-only diffs to keep assertions focused."""
    return {k: mod[k] for k in keys if k in mod}


class TestV1V2Equivalence(unittest.TestCase):
    """v1 flat ⇔ v2 messages-block byte-equal at module-dict level."""

    # ── DDS publisher ──────────────────────────────────────────────
    def test_dds_publisher_equivalence(self):
        v1 = {
            "modules": [{
                "name": "pub",
                "type": "publisher",
                "transport": "dds",
                "message_type": "DemoMsg",
                "dds_type_name": "DemoMsgPubSubType",
                "dds_namespace_cpp": "ibmw::protocols::demo",
            }],
        }
        v2 = {
            "schema_version": 2,
            "messages": {
                "DemoMsg": {
                    "wire": {
                        "dds": {
                            "type_name": "DemoMsgPubSubType",
                            "namespace_cpp": "ibmw::protocols::demo",
                        },
                    },
                },
            },
            "modules": [{
                "name": "pub",
                "type": "publisher",
                "transport": "dds",
                "message": "DemoMsg",
            }],
        }
        m1 = _expand(v1)["modules"][0]
        m2 = _expand(v2)["modules"][0]
        keys = ["name", "type", "transport", "message_type",
                "dds_type_name", "dds_namespace_cpp"]
        self.assertEqual(_module_subset(m1, keys), _module_subset(m2, keys))

    # ── DDS rpc_server (uses dds_service mapping) ─────────────────
    def test_dds_rpc_service_equivalence(self):
        v1 = {
            "modules": [{
                "name": "srv",
                "type": "rpc_server",
                "transport": "dds",
                "message_type": "EchoSvc",
                "dds_service": "EchoSvc",
                "dds_namespace_cpp": "ibmw::protocols::echo",
            }],
        }
        v2 = {
            "schema_version": 2,
            "messages": {
                "EchoSvc": {
                    "wire": {
                        "dds": {
                            "service": "EchoSvc",
                            "namespace_cpp": "ibmw::protocols::echo",
                        },
                    },
                },
            },
            "modules": [{
                "name": "srv",
                "type": "rpc_server",
                "transport": "dds",
                "message": "EchoSvc",
            }],
        }
        m1 = _expand(v1)["modules"][0]
        m2 = _expand(v2)["modules"][0]
        for k in ["name", "type", "transport", "message_type",
                  "dds_service", "dds_namespace_cpp"]:
            self.assertEqual(m1.get(k), m2.get(k), f"key {k!r} mismatch")

    # ── ROS2 transport (full field set) ───────────────────────────
    def test_ros2_publisher_equivalence_full_fields(self):
        v1 = {
            "modules": [{
                "name": "rpub",
                "type": "publisher",
                "transport": "ros2",
                "message_type": "PointMsg",
                "ros2_type_name": "geometry_msgs::msg::Point",
                "ros2_msg_include": "geometry_msgs/msg/point.hpp",
                "ros2_package": "geometry_msgs",
            }],
        }
        v2 = {
            "schema_version": 2,
            "messages": {
                "PointMsg": {
                    "wire": {
                        "ros2": {
                            "type": "geometry_msgs::msg::Point",
                            "include": "geometry_msgs/msg/point.hpp",
                            "package": "geometry_msgs",
                        },
                    },
                },
            },
            "modules": [{
                "name": "rpub",
                "type": "publisher",
                "transport": "ros2",
                "message": "PointMsg",
            }],
        }
        m1 = _expand(v1)["modules"][0]
        m2 = _expand(v2)["modules"][0]
        for k in ["ros2_type_name", "ros2_msg_include", "ros2_package",
                  "message_type"]:
            self.assertEqual(m1.get(k), m2.get(k), f"key {k!r} mismatch")

    # ── iceoryx transport ─────────────────────────────────────────
    def test_iceoryx_subscriber_equivalence(self):
        v1 = {
            "modules": [{
                "name": "isub",
                "type": "loaned_subscriber",
                "transport": "iceoryx",
                "message_type": "PodMsg",
                "iceoryx_msg_type": "PodMsg",
            }],
        }
        v2 = {
            "schema_version": 2,
            "messages": {
                "PodMsg": {
                    "wire": {
                        "iceoryx": {"msg_type": "PodMsg"},
                    },
                },
            },
            "modules": [{
                "name": "isub",
                "type": "loaned_subscriber",
                "transport": "iceoryx",
                "message": "PodMsg",
            }],
        }
        m1 = _expand(v1)["modules"][0]
        m2 = _expand(v2)["modules"][0]
        for k in ["iceoryx_msg_type", "message_type"]:
            self.assertEqual(m1.get(k), m2.get(k), f"key {k!r} mismatch")

    # ── Implicit ros2 transport via direct_ros2_* template ────────
    def test_direct_ros2_publisher_implicit_transport(self):
        # direct_ros2_* templates carry implicit ros2 transport per
        # v2 wire.ros2 must hoist even though the module
        # has no `transport:` field set.
        v2 = {
            "schema_version": 2,
            "messages": {
                "TwistMsg": {
                    "wire": {
                        "ros2": {
                            "type": "geometry_msgs::msg::Twist",
                            "include": "geometry_msgs/msg/twist.hpp",
                            "package": "geometry_msgs",
                        },
                    },
                },
            },
            "modules": [{
                "name": "dpub",
                "type": "direct_ros2_publisher",
                "message": "TwistMsg",
                # no `transport:` — implicit ros2 must still apply
            }],
        }
        m = _expand(v2)["modules"][0]
        self.assertEqual(m.get("ros2_type_name"),
                         "geometry_msgs::msg::Twist")
        self.assertEqual(m.get("ros2_msg_include"),
                         "geometry_msgs/msg/twist.hpp")
        self.assertEqual(m.get("ros2_package"), "geometry_msgs")
        self.assertEqual(m.get("message_type"), "TwistMsg")

    def test_ros2_interop_subscriber_implicit_transport(self):
        # Deprecated alias must still trigger implicit ros2 wire hoist
        # (schema._normalize_module_type_alias rewrites the type later;
        # the expander runs first and uses prefix matching).
        v2 = {
            "schema_version": 2,
            "messages": {
                "M": {"wire": {"ros2": {"type": "x::M",
                                         "include": "x/m.hpp",
                                         "package": "x"}}},
            },
            "modules": [{
                "name": "isub",
                "type": "ros2_interop_subscriber",
                "message": "M",
            }],
        }
        m = _expand(v2)["modules"][0]
        self.assertEqual(m.get("ros2_type_name"), "x::M")


class TestV1PassThrough(unittest.TestCase):
    """v1 yaml (no messages: block) must be a no-op."""

    def test_no_messages_block_is_noop(self):
        before = {
            "modules": [{
                "name": "p", "type": "publisher", "transport": "dds",
                "message_type": "X", "dds_type_name": "XPST",
            }],
        }
        # snapshot
        import copy
        snap = copy.deepcopy(before)
        _expand(before)
        self.assertEqual(before, snap)

    def test_v1_module_takes_precedence_over_v2_block(self):
        # Mixed-mode tolerance: if a module already has the
        # v1 field set, the expander must not clobber it.
        data = {
            "schema_version": 2,
            "messages": {
                "M": {"wire": {"dds": {"type_name": "FromBlock"}}},
            },
            "modules": [{
                "name": "p",
                "type": "publisher",
                "transport": "dds",
                "message": "M",
                "dds_type_name": "ManualOverride",
            }],
        }
        m = _expand(data)["modules"][0]
        self.assertEqual(m["dds_type_name"], "ManualOverride")


class TestErrorPaths(unittest.TestCase):
    def test_unknown_message_ref_errors(self):
        import schema
        data = {
            "schema_version": 2,
            "messages": {"Known": {"wire": {"dds": {"type_name": "X"}}}},
            "modules": [{
                "name": "p", "type": "publisher", "transport": "dds",
                "message": "Unknown",
            }],
        }
        with self.assertRaises(SystemExit):
            schema._expand_v2_to_v1_internal(data)

    def test_messages_must_be_mapping(self):
        import schema
        data = {"messages": ["not", "a", "dict"], "modules": []}
        with self.assertRaises(SystemExit):
            schema._expand_v2_to_v1_internal(data)

    def test_schema_version_must_be_int(self):
        import schema
        with self.assertRaises(SystemExit):
            schema._detect_schema_version({"schema_version": "two"})

    def test_unsupported_schema_version_rejected(self):
        import schema
        with self.assertRaises(SystemExit):
            schema._detect_schema_version({"schema_version": 99})

    def test_default_schema_version_is_v1(self):
        import schema
        v = schema._detect_schema_version({})
        self.assertEqual(v, 1)


if __name__ == "__main__":
    unittest.main()
