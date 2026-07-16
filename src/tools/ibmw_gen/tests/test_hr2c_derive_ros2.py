#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Guard tests for ros2_msg_include / ros2_package auto-derivation.
# helper in schema.py.
#
# The helper peels the C++-qualified message_type into parts and fills the
# two redundant ros2_* fields in-place, honouring any explicit user override
# and leaving non-qualified / non-ROS2 types untouched.

import os
import sys
import unittest

HERE = os.path.abspath(os.path.dirname(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)

from schema import _derive_ros2_fields, _camel_to_snake  # noqa: E402


class HR2CDeriveRos2FieldsTests(unittest.TestCase):

    def test_derives_both_fields_from_canonical_ros2_type(self):
        mod = {"message_type": "geometry_msgs::msg::Point"}
        _derive_ros2_fields(mod)
        self.assertEqual(mod["ros2_package"], "geometry_msgs")
        self.assertEqual(mod["ros2_msg_include"],
                         "geometry_msgs/msg/point.hpp")

    def test_derives_multi_word_camel_snake(self):
        mod = {"message_type": "sensor_msgs::msg::CompressedImage"}
        _derive_ros2_fields(mod)
        self.assertEqual(mod["ros2_package"], "sensor_msgs")
        self.assertEqual(mod["ros2_msg_include"],
                         "sensor_msgs/msg/compressed_image.hpp")

    def test_derives_srv_subnamespace(self):
        mod = {"message_type": "example_interfaces::srv::AddTwoInts"}
        _derive_ros2_fields(mod)
        self.assertEqual(mod["ros2_package"], "example_interfaces")
        self.assertEqual(mod["ros2_msg_include"],
                         "example_interfaces/srv/add_two_ints.hpp")

    def test_user_override_preserved(self):
        mod = {
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "custom/path.hpp",
            "ros2_package": "custom_pkg",
        }
        _derive_ros2_fields(mod)
        self.assertEqual(mod["ros2_msg_include"], "custom/path.hpp")
        self.assertEqual(mod["ros2_package"], "custom_pkg")

    def test_partial_override_fills_the_other(self):
        mod = {
            "message_type": "geometry_msgs::msg::Point",
            "ros2_package": "custom_pkg",
        }
        _derive_ros2_fields(mod)
        self.assertEqual(mod["ros2_package"], "custom_pkg")  # kept
        self.assertEqual(mod["ros2_msg_include"],
                         "geometry_msgs/msg/point.hpp")  # derived

    def test_no_op_when_no_qualifier(self):
        mod = {"message_type": "PodMsg"}
        _derive_ros2_fields(mod)
        self.assertNotIn("ros2_package", mod)
        self.assertNotIn("ros2_msg_include", mod)

    def test_no_op_when_message_type_absent(self):
        mod = {"name": "x"}
        _derive_ros2_fields(mod)
        self.assertNotIn("ros2_package", mod)
        self.assertNotIn("ros2_msg_include", mod)

    def test_camel_to_snake_matches_common_snake(self):
        # byte-equal guard: the local _camel_to_snake used for ros2_msg_include
        # MUST produce the same tokens as generators/_common._snake (used for
        # module / file naming). If these ever diverge, regen byte-equal
        # breaks for any downstream code that composes both.
        from generators._common import _snake as _gen_snake
        for s in ["Point", "CompressedImage", "AddTwoInts", "String",
                  "TwistStamped", "HTTPRequest"]:
            self.assertEqual(_camel_to_snake(s), _gen_snake(s),
                             f"mismatch on {s!r}")


if __name__ == "__main__":
    unittest.main()
