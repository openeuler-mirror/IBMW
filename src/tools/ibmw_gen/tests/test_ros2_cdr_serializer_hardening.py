#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

"""Regression checks for ROS2 CDR serializer hard-fail behavior."""

import pathlib
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SERIALIZER = REPO_ROOT / "extensions" / "transport" / "ros2_types" / "util" / "ros2_cdr_serializer.h"


class Ros2CdrSerializerHardeningTests(unittest.TestCase):
    def test_estimate_size_does_not_guess_when_callbacks_are_missing(self) -> None:
        text = SERIALIZER.read_text(encoding="utf-8")
        estimate_body = text[text.index("static uint32_t EstimateSize") : text.index("template <typename ROS2MsgType>\n  static void SerializeMessage")]

        self.assertNotIn("return 4096", estimate_body)
        self.assertNotIn("kDynamicSerializedTypeSize", estimate_body)
        self.assertIn("refusing to guess serialized size", estimate_body)
        self.assertIn("no generated FastRTPS callbacks", estimate_body)
        self.assertIn("throw std::runtime_error", estimate_body)


if __name__ == "__main__":
    unittest.main()
