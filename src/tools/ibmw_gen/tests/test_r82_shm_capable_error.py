#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Generation-time error for loaned_{publisher,subscriber} declared
# with a non-SHM-capable transport.
#
# _SHM_CAPABLE_TRANSPORTS = {"iceoryx", "dds"}. A loaned
# module with `transport: local` (or any other non-SHM transport) writes an
# `[R8.2-error]` line to stderr BEFORE the existing strict transport
# validator rejects it with `_error` (exit 1). The test therefore expects:
#   - SystemExit on `transport: local` AND an error line on stderr
#   - clean exit (no error) on `transport: dds` (DDS is SHM-capable)
#   - clean exit (no error) on `transport: iceoryx`

import io
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def _minimal_loaned_pub_yaml(transport: str) -> dict:
    return {
        "project": {"name": "r82_test", "namespace": "r82::test"},
        "modules": [{
            "name": "lp",
            "type": "loaned_publisher",
            "transport": transport,
            "topic": "/r82_topic",
            "message_type": "geometry_msgs::msg::Point",
            "ros2_msg_include": "geometry_msgs/msg/point.hpp",
            "ros2_package": "geometry_msgs",
        }],
        "apps": [{
            "name": "r82_app",
            "type": "pubsub",
            "modules": ["lp"],
        }],
    }


def _run_load_and_validate(data: dict):
    """Write `data` to a tempfile, invoke schema.load_and_validate on it,
    and return (raised_systemexit: bool, stderr_text: str)."""
    import schema
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".ibmw.yaml", delete=False, encoding="utf-8"
    ) as f:
        yaml.safe_dump(data, f)
        path = f.name
    err_buf = io.StringIO()
    raised = False
    try:
        with redirect_stderr(err_buf):
            schema.load_and_validate(path)
    except SystemExit:
        raised = True
    return raised, err_buf.getvalue()


class TestR82Error(unittest.TestCase):

    def test_loaned_publisher_local_emits_error(self):
        data = _minimal_loaned_pub_yaml("local")
        raised, stderr_text = _run_load_and_validate(data)
        self.assertTrue(raised, "expected SystemExit (transport=local rejected)")
        self.assertIn("[R8.2-error]", stderr_text)
        self.assertIn("transport=local", stderr_text)

    def test_loaned_publisher_dds_no_error(self):
        data = _minimal_loaned_pub_yaml("dds")
        _, stderr_text = _run_load_and_validate(data)
        self.assertNotIn("[R8.2-error]", stderr_text)

    def test_loaned_publisher_iceoryx_no_error(self):
        data = _minimal_loaned_pub_yaml("iceoryx")
        _, stderr_text = _run_load_and_validate(data)
        self.assertNotIn("[R8.2-error]", stderr_text)

    def test_loaned_subscriber_local_emits_error(self):
        data = _minimal_loaned_pub_yaml("local")
        data["modules"][0]["type"] = "loaned_subscriber"
        raised, stderr_text = _run_load_and_validate(data)
        self.assertTrue(raised)
        self.assertIn("[R8.2-error]", stderr_text)
        self.assertIn("transport=local", stderr_text)


if __name__ == "__main__":
    unittest.main()
