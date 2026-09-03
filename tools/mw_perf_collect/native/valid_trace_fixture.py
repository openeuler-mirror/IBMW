#!/usr/bin/env python3
"""Emit one minimal, repository-format managed DDS trace for native tests."""

import hashlib
import json
import os
import struct


def put(directory_fd: int, name: str, data: bytes) -> None:
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                 0o600, dir_fd=directory_fd)
    try:
        while data:
            count = os.write(fd, data)
            if count <= 0:
                raise OSError("short fixture write")
            data = data[count:]
    finally:
        os.close(fd)


def start_ticks() -> int:
    text = open("/proc/self/stat", encoding="ascii").read()
    return int(text[text.rfind(")") + 2:].split()[19])


def canonical(value: dict) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")


def main() -> None:
    directory_fd = int(os.environ["MW_PERF_COMM_TRACE_DIR_FD"])
    pid, ticks = os.getpid(), start_ticks()
    stream = "11" * 16
    identity = {
        "run_id": os.environ["MW_PERF_RUN_ID"],
        "capability_id": os.environ["MW_PERF_CAPABILITY_ID"],
        "path_id": os.environ["MW_PERF_PATH_ID"],
        "deployment_mode": os.environ["MW_PERF_DEPLOYMENT_MODE"],
    }
    meta = {
        "schema": "mw_perf.dds_comm_trace.meta.v1", **identity, "finalized": True,
        "stream_id": stream, "producer_pid": pid, "producer_start_time_ticks": ticks,
        "topology_generation": os.environ["MW_PERF_TOPOLOGY_GENERATION"],
        "topology_sha256": os.environ["MW_PERF_TOPOLOGY_SHA256"],
        "trace_mode": "full", "sampling_algorithm": 0, "sampling_permyriad": 10000,
    }
    meta_raw = canonical(meta)
    exact_required = os.environ.get("MW_PERF_EXACT_REQUIRED") == "1"
    writer, reader = bytes.fromhex("22" * 16), bytes.fromhex("33" * 16)
    def stage_event(kind: int, start: int, end: int) -> bytes:
        event = bytearray(64)
        struct.pack_into("<QQ", event, 0, start, end)
        event[16:32] = writer
        struct.pack_into("<iI", event, 32, 0, 1)
        if kind != 1:
            event[40:56] = reader
        struct.pack_into("<I", event, 56, 7)
        event[60:64] = bytes((kind, 0, 1, 0))
        return bytes(event)
    events = ([stage_event(1, 10, 20), stage_event(2, 20, 30),
               stage_event(3, 40, 70)] if exact_required else [stage_event(1, 10, 20)])
    payload = b"".join(events)
    header = bytearray(256)
    header[:8] = b"IBMWDCT\0"
    struct.pack_into("<HHHHB", header, 8, 1, 256, 1, 64, 1)
    header[24:40] = bytes.fromhex(stream)
    header[40:72] = hashlib.sha256(meta_raw).digest()
    struct.pack_into("<IIQQQQQQ", header, 72, pid, 0, ticks, 0, len(events), 0, 0, 0)
    struct.pack_into("<Q", header, 128, 0)
    header[136:168] = hashlib.sha256(payload).digest()
    header[168:172] = bytes((2, 0)) + struct.pack("<H", 10000)
    seal = {
        "schema": "mw_perf.dds_comm_trace.seal.v1", **identity, "finalized": True,
        "stream_id": stream, "producer_pid": pid, "producer_start_time_ticks": ticks,
        "trace_mode": "full", "sampling_algorithm": 0, "sampling_permyriad": 10000,
        "segment_count": 1, "record_count": len(events), "exported": len(events), "dropped": 0,
        "invalid_identity": 0, "unsupported_async": 0, "export_discarded": 0,
        "io_error": False, "pending": 0, "active_producers": 0, "sealed": True,
    }
    put(directory_fd, "ibmw_dds_trace.1.a.meta.json", meta_raw)
    put(directory_fd, "ibmw_dds_trace.1.a.00000000.ibct", bytes(header) + payload)
    put(directory_fd, "ibmw_dds_trace.1.a.seal.json", canonical(seal))
    if exact_required:
        exact_meta = {
            "schema": "mw_perf.dds_exact_callback.meta.v1", **identity, "finalized": True,
            "stream_id": stream, "producer_pid": pid, "producer_start_time_ticks": ticks,
            "topology_generation": os.environ["MW_PERF_TOPOLOGY_GENERATION"],
            "topology_sha256": os.environ["MW_PERF_TOPOLOGY_SHA256"],
            "trace_mode": "full", "record_size": 72,
        }
        exact_meta_raw = canonical(exact_meta)
        exact_event = bytearray(72)
        struct.pack_into("<QQ", exact_event, 0, 50, 60)
        exact_event[16:32] = writer
        struct.pack_into("<iI", exact_event, 32, 0, 1)
        exact_event[40:56] = reader
        struct.pack_into("<Q", exact_event, 56, 73)
        exact_event[64:67] = bytes((1, 0, 1))
        exact_header = bytearray(160)
        exact_header[:8] = b"IBMWDCE\0"
        struct.pack_into("<HHHB", exact_header, 8, 1, 160, 72, 1)
        exact_header[16:32] = bytes.fromhex(stream)
        exact_header[32:64] = hashlib.sha256(exact_meta_raw).digest()
        struct.pack_into("<I", exact_header, 64, pid)
        struct.pack_into("<Q", exact_header, 72, ticks)
        struct.pack_into("<Q", exact_header, 80, 1)
        exact_header[128:160] = hashlib.sha256(exact_event).digest()
        exact_seal = {
            "schema": "mw_perf.dds_exact_callback.seal.v1", **identity, "finalized": True,
            "stream_id": stream, "producer_pid": pid, "producer_start_time_ticks": ticks,
            "trace_mode": "full", "record_count": 1, "dropped": 0,
            "invalid_identity": 0, "missing_registration": 0, "unsupported_async": 0,
            "unsupported_multi_handler": 0, "pending": 0, "io_error": False,
            "sealed": True,
        }
        put(directory_fd, "ibmw_dds_exact.1.a.exact.meta.json", exact_meta_raw)
        put(directory_fd, "ibmw_dds_exact.1.a.ibce", bytes(exact_header) + exact_event)
        put(directory_fd, "ibmw_dds_exact.1.a.exact.seal.json", canonical(exact_seal))


if __name__ == "__main__":
    main()
