#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-026: `migrate-yaml --in-place` keeps the first .v1.bak and replaces the
# input atomically.
import os
import stat
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "migrate_yaml.py"

V1 = """project:
  name: demo
modules:
  - name: dpub
    type: direct_iceoryx_publisher
    topic: /twist
    message_type: geometry_msgs::msg::Twist
    iceoryx_msg_type: geometry_msgs/msg/Twist
"""


def _migrate(path):
    return subprocess.run([sys.executable, str(SCRIPT), str(path), "--in-place"],
                          capture_output=True, text=True, timeout=60)


def test_later_run_keeps_the_first_backup(tmp_path):
    path = tmp_path / "demo.ibmw.yaml"
    path.write_text(V1)
    os.chmod(path, 0o640)
    first = _migrate(path)
    assert first.returncode == 0, first.stderr
    bak = tmp_path / "demo.ibmw.yaml.v1.bak"
    assert bak.read_text() == V1
    v2 = path.read_text()
    assert "schema_version: 2" in v2
    assert stat.S_IMODE(path.stat().st_mode) == 0o640

    # The file is v1 again (restored and edited): the first backup stays.
    edited = V1.replace("name: demo", "name: demo2")
    path.write_text(edited)
    second = _migrate(path)
    assert second.returncode != 0
    assert "backup already exists" in second.stderr
    assert bak.read_text() == V1
    assert path.read_text() == edited
    assert sorted(p.name for p in tmp_path.iterdir()) == ["demo.ibmw.yaml", "demo.ibmw.yaml.v1.bak"]


def test_interrupted_write_leaves_the_input_unchanged(tmp_path, monkeypatch):
    sys.path.insert(0, str(SCRIPT.parent))
    import migrate_yaml

    path = tmp_path / "demo.ibmw.yaml"
    path.write_text(V1)

    def fail(_fd):
        raise OSError("disk full")

    monkeypatch.setattr(migrate_yaml.os, "fsync", fail)
    try:
        migrate_yaml._write_atomic(str(path), "x" * 4096)
    except OSError:
        pass
    assert path.read_text() == V1
    assert [p.name for p in tmp_path.iterdir()] == ["demo.ibmw.yaml"]
