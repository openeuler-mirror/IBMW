import hashlib
import json
import struct

import pytest

from mw_perf_collect.exact_trace import (HEADER_SIZE, RECORD_SIZE, parse_binary,
    parse_records, validate_exact_artifact, validate_managed_exact)
import mw_perf_collect.core as core


IDENTITY = {"run_id": "run", "capability_id":
            "dds.ordinary.nonplain.single_driver.sync_callback", "path_id": "path",
            "deployment_mode": "managed"}
STREAM = "aa" * 16


def _managed_exact_files(*, header_pid=123, header_ticks=456, meta_pid=123,
                         meta_ticks=456, seal_pid=123, seal_ticks=456):
    payload = _record()
    meta = {"schema": "mw_perf.dds_exact_callback.meta.v1", **IDENTITY,
            "finalized": True, "stream_id": STREAM, "producer_pid": meta_pid,
            "producer_start_time_ticks": meta_ticks, "topology_generation": "generation-1",
            "topology_sha256": "bb" * 32, "trace_mode": "full", "record_size": 72}
    meta_raw = json.dumps(meta, separators=(",", ":")).encode()
    header = bytearray(HEADER_SIZE)
    header[:8] = b"IBMWDCE\0"
    struct.pack_into("<HHHB", header, 8, 1, HEADER_SIZE, RECORD_SIZE, 1)
    header[16:32] = bytes.fromhex(STREAM)
    header[32:64] = hashlib.sha256(meta_raw).digest()
    struct.pack_into("<I", header, 64, header_pid)
    struct.pack_into("<Q", header, 72, header_ticks)
    struct.pack_into("<Q", header, 80, 1)
    header[128:160] = hashlib.sha256(payload).digest()
    seal = {"schema": "mw_perf.dds_exact_callback.seal.v1", **IDENTITY,
            "finalized": True, "stream_id": STREAM, "producer_pid": seal_pid,
            "producer_start_time_ticks": seal_ticks, "trace_mode": "full",
            "record_count": 1, "dropped": 0, "invalid_identity": 0,
            "missing_registration": 0, "unsupported_async": 0,
            "unsupported_multi_handler": 0, "pending": 0, "io_error": False,
            "sealed": True}
    return {"trace.exact.meta.json": meta_raw, "trace.ibce": bytes(header) + payload,
            "trace.exact.seal.json": json.dumps(seal, separators=(",", ":")).encode()}


def _record(registration=73, status=0):
    value = bytearray(RECORD_SIZE)
    struct.pack_into("<QQ", value, 0, 10, 20)
    value[16:32] = bytes.fromhex("11" * 16)
    struct.pack_into("<iI", value, 32, 0, 9)
    value[40:56] = bytes.fromhex("22" * 16)
    struct.pack_into("<Q", value, 56, registration)
    value[64:67] = bytes((1, status, 1))
    return bytes(value)


def test_exact_roundtrip_preserves_authoritative_identity():
    parsed = validate_exact_artifact(_record())
    assert parsed["status"] == "exact_correlated"
    assert parsed["events"][0]["callback_registration_id"] == 73
    assert parsed["events"][0]["writer_guid"] == "11" * 16


@pytest.mark.parametrize("field", ["registration", "status"])
def test_exact_parser_rejects_missing_identity_or_invalid_status(field):
    raw = _record(**{field: 0 if field == "registration" else 2})
    with pytest.raises(ValueError):
        parse_records(raw)


def test_exact_validator_fails_closed_for_async_or_multi_handler():
    with pytest.raises(ValueError, match="fail-closed"):
        validate_exact_artifact(_record(), unsupported_async=1)
    with pytest.raises(ValueError, match="fail-closed"):
        validate_exact_artifact(_record(), unsupported_multi_handler=1)


def test_managed_exact_binary_roundtrip_checks_header_and_payload_hash():
    payload = _record()
    header = bytearray(HEADER_SIZE)
    header[:8] = b"IBMWDCE\0"
    struct.pack_into("<HHHB", header, 8, 1, HEADER_SIZE, RECORD_SIZE, 1)
    header[16:32] = bytes.fromhex("aa" * 16)
    header[32:64] = bytes.fromhex("bb" * 32)
    struct.pack_into("<I", header, 64, 123)
    struct.pack_into("<Q", header, 72, 456)
    struct.pack_into("<Q", header, 80, 1)
    header[128:160] = hashlib.sha256(payload).digest()
    parsed = parse_binary(bytes(header) + payload)
    assert parsed["record_count"] == 1
    assert parsed["events"][0]["callback_registration_id"] == 73
    broken = bytearray(header) + bytearray(payload)
    broken[-1] ^= 1
    with pytest.raises(ValueError, match="hash"):
        parse_binary(bytes(broken))


def _validate_managed(files, **kwargs):
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v2")
    controls = {"producer_pid": 123, "producer_start_time_ticks": 456,
                "topology_generation": "generation-1", "topology_sha256": "bb" * 32}
    controls.update(kwargs)
    return validate_managed_exact(files, contract["artifact_schemas"],
        core._schema_errors, IDENTITY, **controls)


@pytest.mark.parametrize("field", ["header_pid", "meta_pid", "seal_pid"])
def test_managed_exact_rejects_producer_pid_tampering(field):
    values = {field: 124}
    with pytest.raises(ValueError, match="producer identity|collector control|header mismatch"):
        _validate_managed(_managed_exact_files(**values))


@pytest.mark.parametrize("field", ["header_ticks", "meta_ticks", "seal_ticks"])
def test_managed_exact_rejects_producer_start_ticks_tampering(field):
    values = {field: 457}
    with pytest.raises(ValueError, match="producer identity|collector control|header mismatch"):
        _validate_managed(_managed_exact_files(**values))


def test_managed_exact_requires_manifest_producer_identity_controls():
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v2")
    with pytest.raises(ValueError, match="controls are required"):
        validate_managed_exact(_managed_exact_files(), contract["artifact_schemas"],
                               core._schema_errors, IDENTITY)


@pytest.mark.parametrize("controls", [
    {"producer_pid": 124},
    {"producer_start_time_ticks": 457},
    {"topology_generation": "generation-2"},
    {"topology_sha256": "cc" * 32},
])
def test_managed_exact_rejects_altered_manifest_controls(controls):
    with pytest.raises(ValueError, match="collector control|producer identity"):
        _validate_managed(_managed_exact_files(), **controls)
