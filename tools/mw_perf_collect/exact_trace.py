"""Fail-closed parser/validator for the DDS exact callback artifact."""
from __future__ import annotations

import hashlib
import struct
from typing import Any

from .json_strict import loads as strict_json_loads

RECORD_SIZE = 72
SCHEMA_VERSION = 1
KIND_CALLBACK_SPAN = 1
ZERO_GUID = b"\0" * 16
HEADER_SIZE = 160
MAGIC = b"IBMWDCE\0"


def parse_records(data: bytes) -> list[dict[str, Any]]:
    if len(data) % RECORD_SIZE:
        raise ValueError("exact stream has a partial record")
    result: list[dict[str, Any]] = []
    seen: set[tuple[bytes, int, int, bytes, int]] = set()
    for offset in range(0, len(data), RECORD_SIZE):
        record = data[offset:offset + RECORD_SIZE]
        start, end = struct.unpack_from("<QQ", record, 0)
        writer, reader = record[16:32], record[40:56]
        high = struct.unpack_from("<i", record, 32)[0]
        low = struct.unpack_from("<I", record, 36)[0]
        registration = struct.unpack_from("<Q", record, 56)[0]
        kind, status, schema = record[64], record[65], record[66]
        if (end < start or writer == ZERO_GUID or reader == ZERO_GUID or
                (high == -1 and low == 0) or registration == 0 or
                kind != KIND_CALLBACK_SPAN or status not in (0, 1) or
                schema != SCHEMA_VERSION or any(record[67:])):
            raise ValueError(f"invalid exact record at {offset // RECORD_SIZE}")
        identity = (writer, high, low, reader, registration)
        if identity in seen:
            raise ValueError("duplicate exact callback identity")
        seen.add(identity)
        result.append({"start_ns": start, "end_ns": end,
                       "writer_guid": writer.hex(), "reader_guid": reader.hex(),
                       "sequence_high": high, "sequence_low": low,
                       "callback_registration_id": registration,
                       "kind": "callback_span", "status": status,
                       "schema_version": schema})
    return result


def validate_exact_artifact(data: bytes, *, invalid_identity: int = 0,
                            missing_registration: int = 0,
                            unsupported_async: int = 0,
                            unsupported_multi_handler: int = 0) -> dict[str, Any]:
    if any((invalid_identity, missing_registration, unsupported_async,
            unsupported_multi_handler)):
        raise ValueError("exact artifact has fail-closed exclusions")
    events = parse_records(data)
    return {"status": "exact_correlated", "schema_version": SCHEMA_VERSION,
            "record_size": RECORD_SIZE, "record_count": len(events),
            "events": events}


def parse_binary(data: bytes) -> dict[str, Any]:
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        raise ValueError("exact stream header mismatch")
    version, header_size, record_size, endian = struct.unpack_from("<HHHB", data, 8)
    if (version, header_size, record_size, endian) != (1, HEADER_SIZE, RECORD_SIZE, 1):
        raise ValueError("unsupported exact stream format")
    if data[15] != 0 or any(data[68:72]) or not any(data[16:32]) or not any(data[32:64]):
        raise ValueError("invalid exact stream reserved or identity fields")
    payload = data[HEADER_SIZE:]
    if len(payload) % RECORD_SIZE or hashlib.sha256(payload).digest() != data[128:160]:
        raise ValueError("exact stream partial payload or hash mismatch")
    count = len(payload) // RECORD_SIZE
    if struct.unpack_from("<Q", data, 80)[0] != count:
        raise ValueError("exact stream count mismatch")
    return {"stream_id": data[16:32].hex(), "meta_sha256": data[32:64].hex(),
            "producer_pid": struct.unpack_from("<I", data, 64)[0],
            "producer_start_time_ticks": struct.unpack_from("<Q", data, 72)[0],
            "record_count": count, "dropped": struct.unpack_from("<Q", data, 88)[0],
            "invalid_identity": struct.unpack_from("<Q", data, 96)[0],
            "missing_registration": struct.unpack_from("<Q", data, 104)[0],
            "unsupported_async": struct.unpack_from("<Q", data, 112)[0],
            "unsupported_multi_handler": struct.unpack_from("<Q", data, 120)[0],
            "events": parse_records(payload)}


def validate_managed_exact(files: dict[str, bytes], schemas: dict[str, dict[str, Any]],
                           schema_errors: Any, expected: dict[str, str], *,
                           producer_pid: int | None = None,
                           producer_start_time_ticks: int | None = None,
                           topology_generation: str | None = None,
                           topology_sha256: str | None = None) -> dict[str, Any]:
    meta_names = sorted(n for n in files if n.endswith(".exact.meta.json"))
    stream_names = sorted(n for n in files if n.endswith(".ibce"))
    seal_names = sorted(n for n in files if n.endswith(".exact.seal.json"))
    if len(meta_names) != 1 or len(stream_names) != 1 or len(seal_names) != 1:
        raise ValueError("required exact meta, stream, and seal are missing or duplicated")
    meta = strict_json_loads(files[meta_names[0]])
    seal = strict_json_loads(files[seal_names[0]])
    if schema_errors(meta, schemas["mw_perf.dds_exact_callback.meta.v1"]) or schema_errors(seal, schemas["mw_perf.dds_exact_callback.seal.v1"]):
        raise ValueError("invalid exact meta or seal schema")
    for record in (meta, seal):
        if any(record.get(field) != value for field, value in expected.items()):
            raise ValueError("exact identity mismatch")
    if producer_pid is None or producer_start_time_ticks is None:
        raise ValueError("exact producer identity controls are required")
    controls = {"producer_pid": producer_pid, "producer_start_time_ticks": producer_start_time_ticks,
                "topology_generation": topology_generation, "topology_sha256": topology_sha256}
    if any(v is not None and meta.get(k) != v for k, v in controls.items()):
        raise ValueError("exact meta collector control mismatch")
    parsed = parse_binary(files[stream_names[0]])
    if parsed["stream_id"] != meta["stream_id"] or parsed["meta_sha256"] != hashlib.sha256(files[meta_names[0]]).hexdigest():
        raise ValueError("exact stream/meta hash mismatch")
    producer_fields = ("producer_pid", "producer_start_time_ticks")
    if any(meta[field] != seal[field] or meta[field] != parsed[field]
           or meta[field] != controls[field] for field in producer_fields):
        raise ValueError("exact producer identity mismatch")
    fields = ("producer_pid", "producer_start_time_ticks", "record_count", "dropped",
              "invalid_identity", "missing_registration", "unsupported_async", "unsupported_multi_handler")
    if any(seal[k] != parsed[k] for k in fields): raise ValueError("exact seal/header mismatch")
    if (seal.get("finalized") is not True or seal.get("sealed") is not True
            or seal.get("io_error") is not False or seal.get("pending") != 0):
        raise ValueError("exact seal is not finalized")
    if any(parsed[k] != 0 for k in fields[3:]): raise ValueError("exact loss or unsupported counter is nonzero")
    return {"status": "exact_correlated", "stream_id": parsed["stream_id"],
            "record_count": parsed["record_count"], "events": parsed["events"],
            "dropped": 0, "invalid_identity": 0, "missing_registration": 0,
            "unsupported_async": 0, "unsupported_multi_handler": 0}
