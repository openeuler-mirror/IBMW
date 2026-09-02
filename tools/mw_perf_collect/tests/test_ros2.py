import os
import hashlib
import json
import struct
import subprocess
import time
from types import SimpleNamespace
from pathlib import Path

from mw_perf_collect.adapters.ros2 import Ros2Adapter
from mw_perf_collect import core

ROS2 = "/trusted/ros2"


def _synthetic_elf(*, elf_class=2, endian="<", build_ids=("001122aabbcc",), soname="libfixture.so",
                   malformed_soname=False, truncated_note=False, omit_dt_null=False):
    """Build a small PT_LOAD/PT_NOTE/PT_DYNAMIC ELF fixture locally."""
    is64 = elf_class == 2
    eh_size, ph_size = (64, 56) if is64 else (52, 32)
    phnum = 3
    phoff = eh_size
    note = struct.pack(endian + "III", 4, len(bytes.fromhex(build_ids[0])) if build_ids else 0, 3)
    note += b"GNU\0" + b"\0" * ((-len(note) - 4) % 4)
    if build_ids:
        note += bytes.fromhex(build_ids[0])
        note += b"\0" * ((-len(note)) % 4)
    if len(build_ids) > 1:
        extra = struct.pack(endian + "III", 4, len(bytes.fromhex(build_ids[1])), 3) + b"GNU\0"
        extra += b"\0" * ((-len(extra)) % 4) + bytes.fromhex(build_ids[1])
        extra += b"\0" * ((-len(extra)) % 4)
        note += extra
    if truncated_note:
        note = note[:-1]
    note_offset = phoff + phnum * ph_size
    dyn_offset = note_offset + len(note)
    dynamic_entries = 3 if omit_dt_null else 4
    strtab_offset = dyn_offset + dynamic_entries * (16 if is64 else 8)
    strings = b"\0" + (b"\xff" if malformed_soname else soname.encode("utf-8")) + b"\0"
    dynamic = (struct.pack(endian + "qQ", 5, strtab_offset) +
               struct.pack(endian + "qQ", 10, len(strings)) +
               struct.pack(endian + "qQ", 14, 1)) if is64 else (
               struct.pack(endian + "iI", 5, strtab_offset) + struct.pack(endian + "iI", 10, len(strings)) +
               struct.pack(endian + "iI", 14, 1))
    if not omit_dt_null:
        dynamic += struct.pack(endian + ("qQ" if is64 else "iI"), 0, 0)
    total = strtab_offset + len(strings)
    data = bytearray(total)
    ident = b"\x7fELF" + bytes([elf_class, 1 if endian == "<" else 2, 1]) + b"\0" * 9
    if is64:
        struct.pack_into(endian + "16sHHIQQQIHHHHHH", data, 0, ident, 3, 62, 1, 0, phoff, 0, 0, 64, ph_size, phnum, 0, 0, 0)
        ph = endian + "IIQQQQQQ"
        struct.pack_into(ph, data, phoff, 1, 0, 0, 0, 0, total, total, 1)
        struct.pack_into(ph, data, phoff + ph_size, 4, 0, note_offset, 0, 0, len(note), len(note), 1)
        struct.pack_into(ph, data, phoff + 2 * ph_size, 2, 0, dyn_offset, 0, 0, len(dynamic), len(dynamic), 8)
    else:
        struct.pack_into(endian + "16sHHIIIIIHHHHHH", data, 0, ident, 3, 3, 1, 0, phoff, 0, 0, eh_size, ph_size, phnum, 0, 0, 0)
        ph = endian + "IIIIIIII"
        struct.pack_into(ph, data, phoff, 1, 0, 0, 0, total, total, 0, 1)
        struct.pack_into(ph, data, phoff + ph_size, 4, note_offset, 0, 0, len(note), len(note), 0, 1)
        struct.pack_into(ph, data, phoff + 2 * ph_size, 2, dyn_offset, 0, 0, len(dynamic), len(dynamic), 0, 4)
    data[note_offset:note_offset + len(note)] = note
    data[dyn_offset:dyn_offset + len(dynamic)] = dynamic
    data[strtab_offset:strtab_offset + len(strings)] = strings
    return bytes(data)


def _write_proc_stat(proc, start_ticks):
    (proc / "stat").write_text(f"4242 (fixture) S {' '.join(['0'] * 18)} {start_ticks}\n")


def _mapping_identity(path):
    stat = path.stat()
    return f"{os.major(stat.st_dev):02x}:{os.minor(stat.st_dev):02x}", stat.st_ino


def test_doctor_uses_cli_help_supported_by_humble():
    def runner(args, **kwargs):
        assert tuple(args) == (ROS2, "-h")
        return subprocess.CompletedProcess(args, 0, "usage: ros2 [-h]\n", "")

    result = Ros2Adapter(runner=runner, environ={"ROS_DISTRO": "humble"}).doctor()
    assert result["status"] == "ready"
    assert result["observations"][1]["name"] == "ros2_cli"


def test_doctor_degrades_when_ros2_cli_is_missing():
    def runner(*args, **kwargs):
        raise FileNotFoundError("ros2")

    result = Ros2Adapter(runner=runner, environ={}).doctor()
    assert result["status"] == "degraded"


def test_run_collects_environment_cli_and_graph_without_observer():
    calls = []

    def runner(args, **kwargs):
        calls.append(args)
        output = {
            (ROS2, "-h"): "usage: ros2 [-h]\n",
            (ROS2, "node", "list"): "/talker\n",
            (ROS2, "topic", "list"): "/chatter\n",
            (ROS2, "topic", "info", "-v", "/chatter"): "Type: std_msgs/msg/String\n",
        }[tuple(args)]
        return subprocess.CompletedProcess(args, 0, output, "")

    result = Ros2Adapter(runner=runner, environ={"ROS_DISTRO": "humble", "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp", "ROS_DOMAIN_ID": "7"}).run({"topics": [{"name": "/chatter", "count": 1}]})
    assert result["status"] == "ok"
    assert result["readiness"]["ok"] is True
    assert {ROS2, "node", "list"} == set(calls[0])
    assert all(item["measurement_lane"] == "passive_business_observation" for item in result["observations"])
    assert result["readiness"]["evidence"]["attempts"] == 1
    assert result["readiness"]["evidence"]["first_seen"]["/chatter"] is not None


def test_missing_ros_degrades_and_marks_unsupported():
    def runner(*args, **kwargs):
        raise FileNotFoundError("ros2")

    result = Ros2Adapter(runner=runner, environ={}).run()
    assert result["status"] == "degraded"
    assert result["readiness"]["ok"] is True
    assert any(item["metric_availability"] == "unsupported" for item in result["observations"])
    assert all("metric_availability" in item for item in result["observations"])


def test_process_check_is_optional_and_uses_pid_liveness(monkeypatch):
    monkeypatch.setattr(os, "kill", lambda pid, sig: None)
    monkeypatch.setattr(Ros2Adapter, "_pid_identity", staticmethod(lambda pid: 10))
    result = Ros2Adapter(environ={}).readiness({"pid": 1234}, {"value": {"topics": []}})
    assert result["ok"] is True
    assert result["checks"][0]["name"] == "process_alive"


def test_readiness_polls_until_topic_appears():
    topic_lists = iter(["/chatter\n"])

    def runner(args, **kwargs):
        command = tuple(args)
        if command == (ROS2, "node", "list"):
            output = ""
        elif command == (ROS2, "topic", "list"):
            output = next(topic_lists)
        elif command == (ROS2, "topic", "info", "-v", "/chatter"):
            output = "Type: std_msgs/msg/String\n"
        else:
            raise AssertionError(command)
        return subprocess.CompletedProcess(args, 0, output, "")

    result = Ros2Adapter(runner=runner, environ={}).readiness(
        {"topics": ["/chatter"], "readiness_timeout_seconds": 0.1, "readiness_poll_interval_seconds": 0},
        {"value": {"topics": []}},
    )
    assert result["ok"] is True
    assert result["evidence"]["attempts"] == 2
    assert result["evidence"]["first_seen"]["/chatter"] is not None


def test_readiness_topic_timeout_has_structured_evidence():
    def runner(args, **kwargs):
        return subprocess.CompletedProcess(args, 0, "", "")

    result = Ros2Adapter(runner=runner, environ={}).readiness(
        {"topics": ["/missing"], "readiness_timeout_seconds": 0, "readiness_poll_interval_seconds": 0},
        {"value": {"topics": []}},
    )
    assert result["ok"] is False
    assert result["evidence"] == {
        "attempts": 1,
        "first_seen": {"/missing": None},
        "elapsed": result["evidence"]["elapsed"],
        "failure_reason": "timeout",
    }


def test_readiness_fails_when_process_exits(monkeypatch):
    monkeypatch.setattr(Ros2Adapter, "_pid_identity", staticmethod(lambda pid: 10))
    monkeypatch.setattr(Ros2Adapter, "_process_ready", staticmethod(lambda pid: {"name": "process_alive", "ok": False, "metric_availability": "supported"}))
    result = Ros2Adapter(environ={}).readiness({"pid": 123}, {"value": {"topics": []}})
    assert result["ok"] is False
    assert result["evidence"]["failure_reason"] == "process_exited"


def test_readiness_fails_when_pid_identity_changes(monkeypatch):
    identities = iter([10, 11])
    monkeypatch.setattr(Ros2Adapter, "_pid_identity", staticmethod(lambda pid: next(identities)))
    monkeypatch.setattr(Ros2Adapter, "_process_ready", staticmethod(lambda pid: {"name": "process_alive", "ok": True, "metric_availability": "supported"}))
    result = Ros2Adapter(environ={}).readiness({"pid": 123}, {"value": {"topics": []}})
    assert result["ok"] is False
    assert result["evidence"]["failure_reason"] == "pid_identity_changed"


def test_readiness_cli_timeouts_shrink_with_remaining_deadline():
    timeouts = []

    def runner(args, **kwargs):
        timeouts.append(kwargs["timeout"])
        time.sleep(0.01)
        raise subprocess.TimeoutExpired(args, kwargs["timeout"])

    result = Ros2Adapter(runner=runner, environ={}).readiness(
        {"topics": ["/missing"], "readiness_timeout_seconds": 0.05, "readiness_poll_interval_seconds": 0}
    )

    assert result["ok"] is False
    assert len(timeouts) >= 2
    assert all(0 < later < earlier <= 0.05 for earlier, later in zip(timeouts, timeouts[1:]))


def test_readiness_does_not_start_cli_after_deadline():
    calls = []

    def runner(args, **kwargs):
        calls.append(args)
        return subprocess.CompletedProcess(args, 0, "", "")

    result = Ros2Adapter(runner=runner, environ={}).readiness(
        {"topics": ["/missing"], "readiness_timeout_seconds": 0, "readiness_poll_interval_seconds": 0}
    )

    assert result["ok"] is False
    assert calls == []


def test_command_rejects_success_that_finishes_after_deadline(monkeypatch):
    clock = iter([0.0, 0.2])
    monkeypatch.setattr(time, "monotonic", lambda: next(clock))

    def runner(args, **kwargs):
        return subprocess.CompletedProcess(args, 0, "/chatter\n", "")

    record = Ros2Adapter(runner=runner, environ={})._command(
        ("ros2", "topic", "list"), "ros2_topic_list", deadline=0.1
    )
    assert record["metric_availability"] == "unsupported"
    assert "deadline expired" in record["detail"]


def test_p0a_preflight_collects_maps_library_identity_and_tracing(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"
    proc.mkdir()
    _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib}\n")

    def runner(args, **kwargs):
        outputs = {
            (ROS2, "pkg", "prefix", "rmw_fastrtps_cpp"): "/opt/ros/humble\n",
            (ROS2, "trace", "-h"): "usage: ros2 trace\n",
            (ROS2, "trace", "--list"): "ros2:callback_start\n",
        }
        return subprocess.CompletedProcess(args, 0, outputs[tuple(args)], "")

    record = Ros2Adapter(runner=runner, environ={"ROS_DISTRO": "humble",
        "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp"}, proc_root=tmp_path).preflight({"pid": 4242})
    value = record["value"]
    assert record["metric_availability"] == "supported"
    assert value["middleware_libraries"][0]["status"] == "identity_verified"
    assert value["middleware_libraries"][0]["mapped_identity_matches"] is True
    assert value["middleware_libraries"][0]["mapping"]["perms"] == "r-xp"
    assert value["middleware_libraries"][0]["mapping"]["offset"] == 0
    assert value["process_identity"]["valid"] is True
    assert value["ros2_tracing"]["available"] is False
    assert value["ros2_tracing"]["session_command_usable"] is False
    assert value["claims"]["exact"] is False


def test_p0a_preflight_makes_missing_external_commands_not_available(tmp_path):
    def missing(*args, **kwargs):
        raise FileNotFoundError(str(args[0]))

    record = Ros2Adapter(runner=missing, environ={}, proc_root=tmp_path).preflight({"pid": 999})
    assert record["availability"] == "not_available"
    assert all(item["availability"] == "not_available"
               for item in record["value"]["ros2_tracing"]["commands"])
    assert record["value"]["ros2_pkg_prefix"][0]["availability"] == "not_available"


def test_preflight_pid_reuse_invalidates_runtime_resolution(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"
    proc.mkdir()
    _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib}\n")

    def runner(args, **kwargs):
        _write_proc_stat(proc, 101)
        return subprocess.CompletedProcess(args, 1, "", "unavailable")

    value = Ros2Adapter(runner=runner, environ={}, proc_root=tmp_path).preflight({"pid": 4242})["value"]
    assert value["process_identity"] == {
        "start_ticks_before": 100,
        "start_ticks_after": 101,
        "valid": False,
        "reason": "pid_reused_or_process_identity_changed",
    }
    assert value["process"]["dynamic_library_resolution"]["availability"] == "not_available"
    assert value["middleware_libraries"][0]["sha256"] is not None
    assert value["middleware_libraries"][0]["status"] == "candidate"
    assert value["middleware_libraries"][0]["verification"] == "unverified"


def test_deleted_mapping_is_never_hashed_from_replacement_path(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"replacement-not-mapped-content")
    proc = tmp_path / "4242"
    proc.mkdir()
    _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib} (deleted)\n")

    def runner(args, **kwargs):
        return subprocess.CompletedProcess(args, 1, "", "unavailable")

    library = Ros2Adapter(runner=runner, environ={}, proc_root=tmp_path).preflight(
        {"pid": 4242}
    )["value"]["middleware_libraries"][0]
    assert library["status"] == "unverifiable_deleted"
    assert library["verification"] == "unverifiable_deleted"
    assert library["sha256"] is None
    assert library["mapped_identity_matches"] is False


def test_trace_help_success_but_tracepoint_enumeration_failure_is_unavailable(tmp_path):
    def runner(args, **kwargs):
        if tuple(args) == (ROS2, "trace", "-h"):
            return subprocess.CompletedProcess(args, 0, "usage: ros2 trace\n", "")
        return subprocess.CompletedProcess(args, 1, "", "tracepoint enumeration failed")

    tracing = Ros2Adapter(runner=runner, environ={}, proc_root=tmp_path).preflight()["value"]["ros2_tracing"]
    assert tracing["command_available"] is True
    assert tracing["session_command_usable"] is False
    assert tracing["tracepoints_enumerated"] is False
    assert tracing["required_tracepoints_present"] is False
    assert tracing["available"] is False


def test_maps_identity_mismatch_keeps_readable_library_unverified(tmp_path):
    lib = tmp_path / "librmw_fixture.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"
    proc.mkdir()
    _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode + 1} {lib}\n")

    def runner(args, **kwargs):
        return subprocess.CompletedProcess(args, 1, "", "unavailable")

    library = Ros2Adapter(runner=runner, environ={}, proc_root=tmp_path).preflight(
        {"pid": 4242}
    )["value"]["middleware_libraries"][0]
    assert library["sha256"] is not None
    assert library["mapped_identity_matches"] is False
    assert library["status"] == "candidate"
    assert library["verification"] == "unverified"


def test_readiness_rechecks_pid_after_snapshot_before_success(monkeypatch):
    identities = iter([10, 10, 10, 11])
    monkeypatch.setattr(Ros2Adapter, "_pid_identity", staticmethod(lambda pid: next(identities)))
    monkeypatch.setattr(Ros2Adapter, "_process_ready", staticmethod(lambda pid: {"name": "process_alive", "ok": True, "metric_availability": "supported"}))

    def runner(args, **kwargs):
        output = "/chatter\n" if tuple(args) == (ROS2, "topic", "list") else ""
        return subprocess.CompletedProcess(args, 0, output, "")

    result = Ros2Adapter(runner=runner, environ={}).readiness(
        {"pid": 123, "topics": ["/chatter"], "readiness_timeout_seconds": 0.1}
    )

    assert result["ok"] is False
    assert result["evidence"]["failure_reason"] == "pid_identity_changed"


def test_ros2_subprocess_uses_absolute_command_and_sanitized_environment():
    captured = {}

    def runner(args, **kwargs):
        captured.update(kwargs["env"])
        assert args == [ROS2, "-h"]
        return subprocess.CompletedProcess(args, 0, "usage", "")

    result = Ros2Adapter(runner=runner, environ={
        "PATH": "/trusted/bin", "ROS_DOMAIN_ID": "41", "PYTHONPATH": "/sut/python",
        "PYTHONHOME": "/sut/home", "LD_PRELOAD": "/sut/preload.so",
        "LD_LIBRARY_PATH": "/sut/lib", "LD_AUDIT": "/sut/audit.so",
        "DYLD_INSERT_LIBRARIES": "/sut/dyld", "BASH_ENV": "/sut/bash", "ENV": "/sut/env",
    }).doctor()
    assert result["status"] == "ready"
    assert captured["PATH"] == "/trusted/bin"
    assert captured["ROS_DOMAIN_ID"] == "41"
    assert not Ros2Adapter.UNSAFE_ENV_KEYS.intersection(captured)
    assert not {"LD_AUDIT", "DYLD_INSERT_LIBRARIES", "BASH_ENV", "ENV"}.intersection(captured)


def test_relative_ros2_executable_is_not_trusted():
    called = False

    def runner(*args, **kwargs):
        nonlocal called
        called = True

    result = Ros2Adapter(runner=runner, environ={}, ros2_executable="sut/ros2").doctor()
    assert result["status"] == "degraded"
    assert called is False


def test_tracepoint_probe_uses_humble_list_and_strict_names(tmp_path):
    calls = []

    def runner(args, **kwargs):
        calls.append(tuple(args))
        output = "ros2:rcl_publish\ninvalid\nros2:bad-name\n# comment\n"
        return subprocess.CompletedProcess(args, 0, output, "")

    tracing = Ros2Adapter(runner=runner, environ={}, proc_root=tmp_path).preflight(
        {"required_tracepoints": ["ros2:rcl_publish", "invalid"]})["value"]["ros2_tracing"]
    assert (ROS2, "trace", "--list") in calls
    assert tracing["command_available"] is True
    assert tracing["session_command_usable"] is False
    assert tracing["tracepoints_enumerated"] is True
    assert tracing["tracepoints"] == ["ros2:rcl_publish"]
    assert tracing["required_tracepoints"] == ["ros2:rcl_publish"]
    assert tracing["required_tracepoints_present"] is True
    assert tracing["available"] is False


def test_capability_verification_requires_registered_expected_sha(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"
    proc.mkdir()
    _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib}\n")
    digest = __import__("hashlib").sha256(lib.read_bytes()).hexdigest()

    def runner(args, **kwargs):
        return subprocess.CompletedProcess(args, 1, "", "unavailable")

    adapter = Ros2Adapter(runner=runner, environ={}, proc_root=tmp_path)
    candidate = adapter.preflight({"pid": 4242})["value"]["middleware_libraries"][0]
    assert candidate["verification"] == "identity_verified"
    assert candidate["capability_verified"] is False
    value = adapter.preflight({"pid": 4242, "required_runtime_libraries": [
        {"id": "fastdds", "name": lib.name, "sha256": digest}]})["value"]
    assert value["middleware_libraries"][0]["capability_verified"] is True
    assert value["required_runtime_libraries"]["all_capability_verified"] is True


def test_deleted_required_mapping_cannot_be_capability_verified(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"replacement")
    proc = tmp_path / "4242"
    proc.mkdir()
    _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib} (deleted)\n")
    digest = __import__("hashlib").sha256(lib.read_bytes()).hexdigest()
    value = Ros2Adapter(runner=lambda args, **kwargs: subprocess.CompletedProcess(args, 1, "", ""),
                        environ={}, proc_root=tmp_path).preflight({"pid": 4242,
        "required_runtime_libraries": [{"id": "fastdds", "name": lib.name, "sha256": digest}]})["value"]
    library = value["middleware_libraries"][0]
    assert library["required"] is True
    assert library["capability_verified"] is False
    assert value["required_runtime_libraries"]["all_capability_verified"] is False


def test_required_libraries_are_verified_independently_by_stable_id(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"; proc.mkdir(); _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib}\n")
    digest = __import__("hashlib").sha256(lib.read_bytes()).hexdigest()
    adapter = Ros2Adapter(runner=lambda args, **kwargs: subprocess.CompletedProcess(args, 1, "", ""),
                          environ={}, proc_root=tmp_path)
    value = adapter.preflight({"pid": 4242, "required_runtime_libraries": [
        {"id": "present", "name": lib.name, "sha256": digest},
        {"id": "missing", "name": "librmw_missing.so", "sha256": "0" * 64},
    ]})["value"]["required_runtime_libraries"]
    assert value["matches"] == [
        {"id": "present", "matched": True}, {"id": "missing", "matched": False}]
    assert value["all_capability_verified"] is False


def test_nonempty_library_record_matches_closed_schema(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"; proc.mkdir(); _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(f"7f-8f r-xp 00000000 {device} {inode} {lib}\n")
    value = Ros2Adapter(
        runner=lambda args, **kwargs: subprocess.CompletedProcess(args, 1, "", ""),
        environ={}, proc_root=tmp_path).preflight({"pid": 4242})["value"]
    schema = json.loads((Path(__file__).parents[1] / "schemas/ros2-preflight.schema.v1.json").read_bytes())
    assert core._schema_errors(value, schema) == []
    assert core._schema_errors(value["middleware_libraries"][0], schema["$defs"]["library"], schema) == []


def test_legacy_library_record_remains_v1_schema_compatible():
    schema = json.loads((Path(__file__).parents[1] / "schemas/ros2-preflight.schema.v1.json").read_bytes())
    legacy = {"path": "/x/libfastdds.so", "status": "candidate", "verification": "unverified",
              "observed_in_proc_maps": True, "mapping": {"path": "/x/libfastdds.so", "address": "1-2",
              "perms": "r-xp", "offset": 0, "device": "1:1", "inode": 1, "deleted": False},
              "mapped_identity_matches": False, "sha256": None, "fd_device": None,
              "fd_inode": None, "required": False,
              "expected_identity_match": False, "capability_verified": False}
    assert core._schema_errors(legacy, schema["$defs"]["library"], schema) == []


def test_different_inode_same_basename_rejected(tmp_path):
    first = tmp_path / "first" / "libfastdds.so"
    second = tmp_path / "second" / "libfastdds.so"
    first.parent.mkdir(); second.parent.mkdir()
    first.write_bytes(b"same-library")
    second.write_bytes(b"same-library")
    proc = tmp_path / "4242"; proc.mkdir(); _write_proc_stat(proc, 100)
    first_device, first_inode = _mapping_identity(first)
    second_device, second_inode = _mapping_identity(second)
    (proc / "maps").write_text(
        f"7f-8f r-xp 00000000 {first_device} {first_inode} {first}\n"
        f"8f-9f r-xp 00000000 {second_device} {second_inode} {second}\n")
    digest = __import__("hashlib").sha256(first.read_bytes()).hexdigest()

    value = Ros2Adapter(
        runner=lambda args, **kwargs: subprocess.CompletedProcess(args, 1, "", ""),
        environ={}, proc_root=tmp_path).preflight({"pid": 4242,
            "required_runtime_libraries": [
                {"id": "fastdds", "name": first.name, "sha256": digest}]})["value"]

    assert len(value["middleware_libraries"]) == 2
    assert all(item["capability_verified"] is False
               for item in value["middleware_libraries"])
    assert value["required_runtime_libraries"]["matches"] == [
        {"id": "fastdds", "matched": False, "reason": "ambiguous_runtime_match"}]
    assert value["required_runtime_libraries"]["all_capability_verified"] is False


def test_same_inode_multiple_segments_accepted(tmp_path):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture-library")
    proc = tmp_path / "4242"; proc.mkdir(); _write_proc_stat(proc, 100)
    device, inode = _mapping_identity(lib)
    (proc / "maps").write_text(
        f"7f-8f r--p 00000000 {device} {inode} {lib}\n"
        f"8f-9f r-xp 00001000 {device} {inode} {lib}\n")
    digest = __import__("hashlib").sha256(lib.read_bytes()).hexdigest()

    value = Ros2Adapter(
        runner=lambda args, **kwargs: subprocess.CompletedProcess(args, 1, "", ""),
        environ={}, proc_root=tmp_path).preflight({"pid": 4242,
            "required_runtime_libraries": [
                {"id": "fastdds", "name": lib.name, "sha256": digest}]})["value"]

    assert len(value["middleware_libraries"]) == 1
    assert value["middleware_libraries"][0]["capability_verified"] is True
    assert value["required_runtime_libraries"]["matches"] == [
        {"id": "fastdds", "matched": True}]
    assert value["required_runtime_libraries"]["all_capability_verified"] is True


def test_required_library_declarations_reject_duplicate_ids_and_ambiguous_selectors():
    adapter = Ros2Adapter(runner=lambda args, **kwargs: subprocess.CompletedProcess(args, 1, "", ""), environ={})
    for declarations in (
        [{"id": "same", "name": "a.so", "sha256": "0" * 64},
         {"id": "same", "name": "b.so", "sha256": "1" * 64}],
        [{"id": "one", "name": "a.so", "sha256": "0" * 64},
         {"id": "two", "path": "/other/a.so", "sha256": "1" * 64}],
        [{"id": "one", "name": "a.so", "soname": "shared.so"},
         {"id": "two", "name": "shared.so", "sha256": "1" * 64}],
    ):
        with __import__("pytest").raises(ValueError):
            adapter.preflight({"required_runtime_libraries": declarations})


def test_hash_rejects_file_metadata_change_during_read(tmp_path, monkeypatch):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture")
    stat = lib.stat()
    mapping = {"path": str(lib), "device":
        f"{os.major(stat.st_dev):02x}:{os.minor(stat.st_dev):02x}",
        "inode": stat.st_ino, "deleted": False}
    real_fstat = os.fstat
    calls = 0

    def changing_fstat(fd):
        nonlocal calls
        current = real_fstat(fd)
        calls += 1
        values = {name: getattr(current, name) for name in dir(current)
                  if name.startswith("st_")}
        if calls == 2:
            values["st_ctime_ns"] += 1
        return SimpleNamespace(**values)

    monkeypatch.setattr(os, "fstat", changing_fstat)
    identity = Ros2Adapter._mapped_file_identity(mapping)
    assert identity["sha256"] is None
    assert identity["mapped_identity_matches"] is False
    assert "unstable" in identity["detail"]


def test_hash_rejects_growth_without_reading_past_initial_size(tmp_path, monkeypatch):
    lib = tmp_path / "libfastdds.so"
    lib.write_bytes(b"fixture")
    stat = lib.stat()
    mapping = {"path": str(lib), "device":
        f"{os.major(stat.st_dev):02x}:{os.minor(stat.st_dev):02x}",
        "inode": stat.st_ino, "deleted": False}
    real_read = os.read
    real_fstat = os.fstat
    bytes_read = 0
    stat_calls = 0

    def bounded_read(fd, size):
        nonlocal bytes_read
        chunk = real_read(fd, size)
        bytes_read += len(chunk)
        return chunk

    def growing_fstat(fd):
        nonlocal stat_calls
        current = real_fstat(fd)
        stat_calls += 1
        if stat_calls == 2:
            values = {name: getattr(current, name) for name in dir(current)
                      if name.startswith("st_")}
            values["st_size"] += 1
            return SimpleNamespace(**values)
        return current

    monkeypatch.setattr(os, "read", bounded_read)
    monkeypatch.setattr(os, "fstat", growing_fstat)
    identity = Ros2Adapter._mapped_file_identity(mapping, max_bytes=32,
                                                  remaining_bytes=[32])
    assert identity["sha256"] is None
    assert identity["detail"] == "mapped file grew while hashing"
    assert bytes_read == len(b"fixture")


def test_distinct_exact_path_declarations_may_share_basename(tmp_path):
    first = tmp_path / "one" / "shared.so"
    second = tmp_path / "two" / "shared.so"
    first.parent.mkdir(); second.parent.mkdir()
    specs = Ros2Adapter._expected_library_specs({"required_runtime_libraries": [
        {"id": "one", "path": str(first), "sha256": "0" * 64},
        {"id": "two", "path": str(second), "sha256": "1" * 64},
    ]})
    assert [item["id"] for item in specs] == ["one", "two"]


def test_elf64_le_provenance_success():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf())
    assert result == {"soname": "libfixture.so", "build_id": "001122aabbcc",
                      "elf_metadata_status": "ok", "elf_metadata_reason": None}


def test_elf32_be_provenance_success():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf(elf_class=1, endian=">"))
    assert result["soname"] == "libfixture.so" and result["build_id"] == "001122aabbcc"


def test_elf_provenance_no_build_id():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf(build_ids=()))
    assert result["build_id"] is None and result["elf_metadata_status"] == "ok"


def test_elf_provenance_truncated_note_fails_explicitly():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf(truncated_note=True))
    assert result["elf_metadata_status"] == "failed" and result["elf_metadata_reason"] == "truncated_note"


def test_elf_provenance_conflicting_ids_fail_explicitly():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf(build_ids=("aa", "bb")))
    assert result["elf_metadata_status"] == "failed" and result["elf_metadata_reason"] == "conflicting_build_ids"


def test_elf_provenance_malformed_soname_fails_explicitly():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf(malformed_soname=True))
    assert result["elf_metadata_status"] == "failed" and result["elf_metadata_reason"] == "invalid_soname_utf8"


def test_elf_provenance_missing_dynamic_terminator_fails_explicitly():
    result = Ros2Adapter._parse_elf_provenance(_synthetic_elf(omit_dt_null=True))
    assert result["elf_metadata_status"] == "failed"
    assert result["elf_metadata_reason"] == "missing_dynamic_terminator"


def test_expected_elf_provenance_match():
    library = {"path": "/x/libfixture.so", "verification": "identity_verified",
               "sha256": "a" * 64, **Ros2Adapter._parse_elf_provenance(_synthetic_elf())}
    Ros2Adapter._apply_expected_identity(library, [{"id": "x", "name": "libfixture.so",
        "sha256": "a" * 64, "build_id": "001122aabbcc", "soname": "libfixture.so"}])
    assert library["capability_verified"] is True


def test_expected_elf_provenance_mismatch_fails_closed():
    library = {"path": "/x/libfixture.so", "verification": "identity_verified", "sha256": "a" * 64,
               **Ros2Adapter._parse_elf_provenance(_synthetic_elf())}
    Ros2Adapter._apply_expected_identity(library, [{"id": "x", "name": "libfixture.so", "build_id": "ff"}])
    assert library["capability_verified"] is False


def test_expected_elf_provenance_missing_fails_closed():
    library = {"path": "/x/libfixture.so", "verification": "identity_verified", "sha256": "a" * 64,
               "soname": None, "build_id": None}
    Ros2Adapter._apply_expected_identity(library, [{"id": "x", "name": "libfixture.so", "soname": "libfixture.so"}])
    assert library["capability_verified"] is False
