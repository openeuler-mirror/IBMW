import hashlib
import json
import os
import struct
import sys
from pathlib import Path

import pytest

import mw_perf_collect.core as core
import mw_perf_collect.dds_trace as dds_trace
from mw_perf_collect.dds_trace import (binary_record_count, confirm_directory_unchanged, parse_binary,
    derive_communication_delivery, derive_communication_latency, derive_hook_callgraph, derive_identity_trace,
    derive_normalized_results, derive_trace_quality, scan_directory_fd, validate_directory_files,
    write_directory_finalization)


IDENTITY = {"run_id": "run", "capability_id":
            "dds.ordinary.nonplain.single_driver.sync_callback", "path_id": "path",
            "deployment_mode": "managed"}
STREAM = "11" * 16


def _meta(stream=STREAM, pid=123, ticks=456):
    return {"schema": "mw_perf.dds_comm_trace.meta.v1", **IDENTITY, "finalized": True,
        "stream_id": stream, "producer_pid": pid, "producer_start_time_ticks": ticks,
        "topology_generation": "generation-1", "topology_sha256": "22" * 32,
        "trace_mode": "full", "sampling_algorithm": 0, "sampling_permyriad": 10000}


def _record(sequence=1, kind=1, reader=None, status=0, *, writer="33" * 16,
            start=10, end=20):
    value = bytearray(64)
    struct.pack_into("<QQ", value, 0, start, end)
    value[16:32] = bytes.fromhex(writer)
    struct.pack_into("<iI", value, 32, 0, sequence)
    value[40:56] = bytes.fromhex(reader or ("00" * 16 if kind == 1 else "44" * 16))
    struct.pack_into("<I", value, 56, 7)
    value[60:64] = bytes((kind, status, 1, 0))
    return bytes(value)


def _binary(meta_raw, *, segment=0, ordinal=0, records=None, dropped=0, stream=STREAM):
    payload = b"".join(records if records is not None else [_record()])
    header = bytearray(256)
    header[:8] = b"IBMWDCT\0"
    struct.pack_into("<HHHHB", header, 8, 1, 256, 1, 64, 1)
    header[24:40] = bytes.fromhex(stream)
    header[40:72] = hashlib.sha256(meta_raw).digest()
    struct.pack_into("<IIQQQQQQ", header, 72, 123, segment, 456, ordinal,
                     len(payload) // 64, 0, dropped, 0)
    struct.pack_into("<Q", header, 128, 0)
    header[136:168] = hashlib.sha256(payload).digest()
    header[168] = 2
    header[169] = 0
    struct.pack_into("<H", header, 170, 10000)
    return bytes(header) + payload


def _files(*, dropped=0, second=False, records=None):
    meta_raw = json.dumps(_meta(), separators=(",", ":")).encode()
    binaries = {"stream.meta.json": meta_raw,
                "stream.000000.ibct": _binary(meta_raw, dropped=dropped, records=records)}
    count = len(records) if records is not None else 1
    if second:
        binaries["stream.000001.ibct"] = _binary(
            meta_raw, segment=1, ordinal=1, records=[_record(2)], dropped=dropped)
        count = 2
    seal = {"schema": "mw_perf.dds_comm_trace.seal.v1", **IDENTITY,
        "finalized": True, "stream_id": STREAM, "producer_pid": 123,
        "producer_start_time_ticks": 456, "trace_mode": "full",
        "sampling_algorithm": 0, "sampling_permyriad": 10000,
        "segment_count": 2 if second else 1, "record_count": count, "exported": count,
        "dropped": dropped, "invalid_identity": 0, "unsupported_async": 0,
        "export_discarded": 0, "io_error": False, "pending": 0,
        "active_producers": 0, "sealed": True}
    binaries["stream.seal.json"] = json.dumps(seal, separators=(",", ":")).encode()
    return binaries


def _exact_record(*, writer="33" * 16, sequence=1, reader="44" * 16,
                  registration=73, status=0, start=50, end=60):
    value = bytearray(72)
    struct.pack_into("<QQ", value, 0, start, end)
    value[16:32] = bytes.fromhex(writer)
    struct.pack_into("<iI", value, 32, 0, sequence)
    value[40:56] = bytes.fromhex(reader)
    struct.pack_into("<Q", value, 56, registration)
    value[64:67] = bytes((1, status, 1))
    return bytes(value)


def _exact_files(*, stage_records=None, exact_records=None, stage_segments=None,
                 exact_stream=STREAM):
    if stage_records is None:
        stage_records = [
            _record(kind=1, start=10, end=20),
            _record(kind=2, start=20, end=30),
            _record(kind=3, start=40, end=70),
        ]
    if exact_records is None:
        exact_records = [_exact_record()]
    files = _files(records=stage_records)
    if stage_segments is not None:
        meta_raw = files["stream.meta.json"]
        del files["stream.000000.ibct"]
        ordinal = 0
        for segment, records in enumerate(stage_segments):
            files[f"stream.{segment:06d}.ibct"] = _binary(
                meta_raw, segment=segment, ordinal=ordinal, records=records)
            ordinal += len(records)
        seal = json.loads(files["stream.seal.json"])
        seal["segment_count"] = len(stage_segments)
        seal["record_count"] = seal["exported"] = ordinal
        files["stream.seal.json"] = json.dumps(seal, separators=(",", ":")).encode()
    payload = b"".join(exact_records)
    meta = {"schema": "mw_perf.dds_exact_callback.meta.v1", **IDENTITY,
        "finalized": True, "stream_id": exact_stream, "producer_pid": 123,
        "producer_start_time_ticks": 456, "topology_generation": "generation-1",
        "topology_sha256": "22" * 32, "trace_mode": "full", "record_size": 72}
    meta_raw = json.dumps(meta, separators=(",", ":")).encode()
    header = bytearray(160)
    header[:8] = b"IBMWDCE\0"
    struct.pack_into("<HHHB", header, 8, 1, 160, 72, 1)
    header[16:32] = bytes.fromhex(exact_stream)
    header[32:64] = hashlib.sha256(meta_raw).digest()
    struct.pack_into("<I", header, 64, 123)
    struct.pack_into("<Q", header, 72, 456)
    struct.pack_into("<Q", header, 80, len(exact_records))
    header[128:160] = hashlib.sha256(payload).digest()
    seal = {"schema": "mw_perf.dds_exact_callback.seal.v1", **IDENTITY,
        "finalized": True, "stream_id": exact_stream, "producer_pid": 123,
        "producer_start_time_ticks": 456, "trace_mode": "full",
        "record_count": len(exact_records), "dropped": 0, "invalid_identity": 0,
        "missing_registration": 0, "unsupported_async": 0,
        "unsupported_multi_handler": 0, "pending": 0, "io_error": False,
        "sealed": True}
    files.update({"trace.exact.meta.json": meta_raw,
                  "trace.ibce": bytes(header) + payload,
                  "trace.exact.seal.json": json.dumps(seal, separators=(",", ":")).encode()})
    return files


def _validate(files, **kwargs):
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v2")
    return validate_directory_files(files, contract["artifact_schemas"],
        core._schema_errors, IDENTITY, run_type="stage_run", measurement_kind="stage_only",
        producer_pid=123, producer_start_time_ticks=456,
        topology_generation="generation-1", topology_sha256="22" * 32, **kwargs)


def _validate_exact(files):
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v2")
    return validate_directory_files(files, contract["artifact_schemas"],
        core._schema_errors, IDENTITY, run_type="exact_run",
        measurement_kind="exact_correlated", producer_pid=123,
        producer_start_time_ticks=456, topology_generation="generation-1",
        topology_sha256="22" * 32)


def _derive_identity(files):
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v4")
    return derive_identity_trace(files, contract["artifact_schemas"], core._schema_errors,
        IDENTITY, producer_pid=123, producer_start_time_ticks=456,
        topology_generation="generation-1", topology_sha256="22" * 32)


def _normalized_inputs(files):
    identity = _derive_identity(files)
    declarations = [
        {"role": "sut", "name": "candidate", "source": "exe",
         "expected_path": "/offline/candidate", "sha256": "aa" * 32,
         "expected_device": 1, "expected_inode": 2, "expected_size": 3},
        {"role": "extension", "name": "extension", "source": "maps",
         "expected_path": "/offline/extension.so", "sha256": "bb" * 32,
         "expected_device": 1, "expected_inode": 3, "expected_size": 4},
    ]
    resolution = {"schema": "mw_perf.p0a.resolution.v2", **IDENTITY,
        "declaration_sha256": hashlib.sha256(dds_trace._canonical_bytes(
            declarations)).hexdigest(), "declarations": declarations,
        "matches": [
            {"role": item["role"], "name": item["name"], "source": item["source"],
             "expected_path": item["expected_path"], "expected_sha256": item["sha256"],
             "expected_device": item["expected_device"],
             "expected_inode": item["expected_inode"], "expected_size": item["expected_size"],
             "observed": [("cc" if item["role"] == "sut" else "dd") * 32]}
            for item in declarations]}
    resolution_bytes = dds_trace._canonical_bytes(resolution, newline=True)
    hook = derive_hook_callgraph(identity, resolution, resolution_bytes,
                                 registration_contract_sha256="ee" * 32)
    return identity, resolution, resolution_bytes, hook


def test_binary_parser_validates_header_payload_and_events():
    files = _files(); raw = files["stream.000000.ibct"]
    assert binary_record_count(raw) == 1
    assert parse_binary(raw)["events"][0]["kind"] == "publish"
    for offset in (0, 8, 10, 12, 14, 16, 17, 136):
        changed = bytearray(raw); changed[offset] ^= 1
        with pytest.raises(ValueError):
            parse_binary(bytes(changed))
    with pytest.raises(ValueError):
        parse_binary(raw + b"x")


def test_binary_parser_rejects_count_timestamp_guid_and_duplicate_stage():
    files = _files(); raw = files["stream.000000.ibct"]
    changed = bytearray(raw); struct.pack_into("<Q", changed, 96, 2)
    with pytest.raises(ValueError): parse_binary(bytes(changed))
    meta_raw = files["stream.meta.json"]
    with pytest.raises(ValueError):
        parse_binary(_binary(meta_raw, records=[_record(), _record()]))
    bad = bytearray(_record()); struct.pack_into("<Q", bad, 8, 9)
    with pytest.raises(ValueError): parse_binary(_binary(meta_raw, records=[bytes(bad)]))
    bad = bytearray(_record()); bad[16:32] = b"\0" * 16
    with pytest.raises(ValueError): parse_binary(_binary(meta_raw, records=[bytes(bad)]))


def test_binary_parser_accepts_failure_status_and_rejects_unknown_status():
    meta_raw = _files()["stream.meta.json"]
    parsed = parse_binary(_binary(meta_raw, records=[_record(status=1)]))
    assert parsed["events"][0]["status"] == 1
    for status in (2, 255):
        with pytest.raises(ValueError, match="control fields"):
            parse_binary(_binary(meta_raw, records=[_record(status=status)]))


def test_binary_parser_requires_frozen_sampling_algorithm_and_rate():
    raw = bytearray(_files()["stream.000000.ibct"])
    raw[168] = 1
    for algorithm, permyriad in ((2, 625), (1, 624), (1, 626)):
        changed = bytearray(raw); changed[169] = algorithm
        struct.pack_into("<H", changed, 170, permyriad)
        with pytest.raises(ValueError, match="sampling"):
            parse_binary(bytes(changed))
    raw[169] = 1; struct.pack_into("<H", raw, 170, 625)
    assert parse_binary(bytes(raw))["mode"] == "sampled"


def test_directory_validation_reconciles_segments_meta_seal_and_loss():
    result = _validate(_files(second=True))
    assert result["record_count"] == 2 and result["segment_count"] == 2
    files = _files(second=True)
    changed = bytearray(files["stream.000001.ibct"]); struct.pack_into("<Q", changed, 88, 9)
    payload = changed[256:]; changed[136:168] = hashlib.sha256(payload).digest()
    files["stream.000001.ibct"] = bytes(changed)
    with pytest.raises(ValueError, match="ordinal"): _validate(files)
    with pytest.raises(ValueError, match="zero-loss"): _validate(_files(dropped=1))
    files = _files(second=True)
    changed = bytearray(files["stream.000001.ibct"]); struct.pack_into("<Q", changed, 104, 1)
    files["stream.000001.ibct"] = bytes(changed)
    with pytest.raises(ValueError, match="counters"): _validate(files)


def test_directory_validation_rejects_meta_swap_seal_mismatch_and_exact_claims():
    files = _files(); swapped = dict(files)
    swapped["stream.meta.json"] = json.dumps(_meta(pid=999), separators=(",", ":")).encode()
    with pytest.raises(ValueError): _validate(swapped)
    mismatch = _files(); seal = json.loads(mismatch["stream.seal.json"])
    seal["exported"] = 2; mismatch["stream.seal.json"] = json.dumps(seal).encode()
    with pytest.raises(ValueError, match="seal/header"): _validate(mismatch)
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v2")
    with pytest.raises(ValueError, match="required exact"):
        validate_directory_files(_files(), contract["artifact_schemas"], core._schema_errors,
            IDENTITY, run_type="exact_run", measurement_kind="exact_correlated")
    with pytest.raises(ValueError, match="stage_only"):
        validate_directory_files(_files(), contract["artifact_schemas"], core._schema_errors,
            IDENTITY, run_type="stage_run", measurement_kind="exact_correlated")
    with pytest.raises(ValueError, match="stage_only"):
        validate_directory_files(_files(), contract["artifact_schemas"], core._schema_errors,
            IDENTITY, run_type="stage_run", measurement_kind="arbitrary")
    ordinal = _files(); changed = bytearray(ordinal["stream.000000.ibct"])
    struct.pack_into("<Q", changed, 88, 1); ordinal["stream.000000.ibct"] = bytes(changed)
    with pytest.raises(ValueError, match="not zero"):
        _validate(ordinal)


def test_exact_directory_validation_correlates_complete_logical_chain():
    result = _validate_exact(_exact_files())
    assert result["measurement_kind"] == "exact_correlated"
    assert result["record_count"] == 1


def test_exact_directory_validation_rejects_stage_stream_identity_mismatch():
    with pytest.raises(ValueError, match="exact stream identity"):
        _validate_exact(_exact_files(exact_stream="aa" * 16))


def test_exact_correlation_rejects_empty_complete_chain():
    with pytest.raises(ValueError, match="at least one exact delivery"):
        _validate_exact(_exact_files(stage_records=[], exact_records=[]))


def test_exact_correlation_handles_reordered_samples_across_segments():
    stages = [
        _record(sequence=2, kind=2, start=115, end=130),
        _record(sequence=1, kind=1, start=10, end=25),
        _record(sequence=2, kind=1, start=110, end=125),
        _record(sequence=1, kind=3, start=40, end=70),
        _record(sequence=1, kind=2, start=20, end=30),
        _record(sequence=2, kind=3, start=140, end=170),
    ]
    exact = [
        _exact_record(sequence=2, registration=74, start=150, end=160),
        _exact_record(sequence=1, registration=73, start=50, end=60),
    ]
    result = _validate_exact(_exact_files(
        exact_records=exact, stage_segments=[stages[:2], stages[2:4], stages[4:]]))
    assert result["measurement_kind"] == "exact_correlated"
    assert result["record_count"] == 2


@pytest.mark.parametrize("exact_record", [
    _exact_record(writer="55" * 16),
    _exact_record(sequence=2),
    _exact_record(reader="55" * 16),
], ids=["writer", "sequence", "reader"])
def test_exact_correlation_rejects_mismatched_delivery_identity(exact_record):
    with pytest.raises(ValueError, match="match exact stream"):
        _validate_exact(_exact_files(exact_records=[exact_record]))


@pytest.mark.parametrize("missing_kind", [1, 2, 3], ids=["publish", "receive", "dispatch"])
def test_exact_correlation_rejects_missing_stage(missing_kind):
    stages = [
        _record(kind=1, start=10, end=20),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=40, end=70),
    ]
    with pytest.raises(ValueError, match="match exact stream"):
        _validate_exact(_exact_files(
            stage_records=[record for kind, record in zip((1, 2, 3), stages)
                           if kind != missing_kind]))


def test_exact_correlation_rejects_extra_stage_and_duplicate_delivery_key():
    stages = [
        _record(kind=1, start=10, end=20),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=40, end=70),
        _record(sequence=2, kind=1, start=10, end=20),
    ]
    with pytest.raises(ValueError, match="match exact stream"):
        _validate_exact(_exact_files(stage_records=stages))
    with pytest.raises(ValueError, match="duplicate exact DeliveryKey"):
        _validate_exact(_exact_files(exact_records=[
            _exact_record(registration=73), _exact_record(registration=74)]))


def test_exact_correlation_accepts_two_reader_fanout():
    stages = [
        _record(kind=1, start=10, end=20),
        _record(kind=2, reader="44" * 16, start=20, end=30),
        _record(kind=3, reader="44" * 16, start=40, end=70),
        _record(kind=2, reader="55" * 16, start=20, end=30),
        _record(kind=3, reader="55" * 16, start=40, end=70),
    ]
    exact = [_exact_record(reader="44" * 16), _exact_record(reader="55" * 16)]
    result = _validate_exact(_exact_files(stage_records=stages, exact_records=exact))
    assert result["measurement_kind"] == "exact_correlated"
    assert result["record_count"] == 2


def test_identity_v2_derivation_single_delivery_and_source_set():
    files = _exact_files(exact_records=[_exact_record(registration=1)])
    derived = _derive_identity(files)
    reordered = _derive_identity(dict(reversed(list(files.items()))))
    assert reordered["source_set"] == derived["source_set"]
    assert reordered["source_set_sha256"] == derived["source_set_sha256"]
    assert reordered["bytes"] == derived["bytes"]
    assert derived["record_count"] == 1
    record = derived["records"][0]
    assert record["callback_registration_id"] == 1
    assert record["stream_id"] == STREAM
    assert (record["publish_start_ns"], record["receive_start_ns"],
            record["dispatch_start_ns"], record["callback_start_ns"],
            record["callback_end_ns"], record["dispatch_end_ns"]) == (10, 20, 40, 50, 60, 70)
    inventory = derived["source_set"]
    assert {item["relative_filename"] for item in inventory} == set(files)
    assert {item["record_count"] for item in inventory
            if item["relative_filename"].endswith((".meta.json", ".seal.json"))} == {1}
    assert next(item for item in inventory if item["relative_filename"].endswith(".ibct"))["record_count"] == 3
    assert next(item for item in inventory if item["relative_filename"].endswith(".ibce"))["record_count"] == 1
    canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":")).encode()
    assert derived["source_set_sha256"] == hashlib.sha256(canonical).hexdigest()
    assert derived["bytes"] == json.dumps(record, sort_keys=True, separators=(",", ":")).encode() + b"\n"


def test_identity_v2_fanout_has_canonical_delivery_order():
    stages = [_record(kind=1, start=10, end=20)]
    for reader in ("55" * 16, "44" * 16):
        stages += [_record(kind=2, reader=reader, start=20, end=30),
                   _record(kind=3, reader=reader, start=40, end=70)]
    exact = [_exact_record(reader="55" * 16, registration=2),
             _exact_record(reader="44" * 16, registration=1)]
    records = _derive_identity(_exact_files(stage_records=stages,
                                            exact_records=exact))["records"]
    assert [item["reader_guid"] for item in records] == ["44" * 16, "55" * 16]
    assert [item["callback_registration_id"] for item in records] == [1, 2]


def test_hook_callgraph_v2_derivation_is_deterministic_and_semantic_only():
    identity = _derive_identity(_exact_files(exact_records=[_exact_record(registration=73)]))
    declaration = {"role": "sut", "name": "candidate", "source": "exe",
        "expected_path": "/offline/candidate", "sha256": "aa" * 32,
        "expected_device": 1, "expected_inode": 2, "expected_size": 3}
    extension = {"role": "extension", "name": "ibmw_dds_extension", "source": "maps",
        "expected_path": "/offline/extension.so", "sha256": "dd" * 32,
        "expected_device": 1, "expected_inode": 3, "expected_size": 4}
    declarations = [declaration, extension]
    resolution = {"schema": "mw_perf.p0a.resolution.v2", **IDENTITY,
        "declaration_sha256": hashlib.sha256(dds_trace._canonical_bytes(
            declarations)).hexdigest(), "declarations": declarations,
        "matches": [{"role": "sut", "name": "candidate", "source": "exe",
            "expected_path": "/offline/candidate", "expected_sha256": "aa" * 32,
            "expected_device": 1, "expected_inode": 2, "expected_size": 3,
            "observed": ["bb" * 32]},
            {"role": "extension", "name": "ibmw_dds_extension", "source": "maps",
             "expected_path": "/offline/extension.so", "expected_sha256": "dd" * 32,
             "expected_device": 1, "expected_inode": 3, "expected_size": 4,
             "observed": ["ee" * 32]}]}
    resolution_bytes = dds_trace._canonical_bytes(resolution, newline=True)
    first = derive_hook_callgraph(identity, resolution, resolution_bytes,
                                  registration_contract_sha256="cc" * 32)
    second = derive_hook_callgraph(identity, resolution, resolution_bytes,
                                   registration_contract_sha256="cc" * 32)
    assert first["bytes"] == second["bytes"]
    assert first["bytes"].endswith(b"\n")
    value = first["value"]
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v5")
    assert not core._schema_errors(
        value, contract["artifact_schemas"]["mw_perf.p0a.hook_callgraph.v2"])
    assert value["claim"] == {
        "kind": "semantic_boundary_chain", "threat_model": "cooperative_sut",
        "coverage": "successful_exact_deliveries_only",
        "dynamic_stack_observation": "none",
        "runtime_function_addresses_observed": False,
        "source_to_build_provenance": "not_proven"}
    assert [item["logical_boundary"] for item in value["boundary_catalog"]] == [
        "publish_write", "take_receive", "sync_dispatch", "final_callback_invocation"]
    assert value["stage_record_count"] == 3
    assert value["callback_record_count"] == value["delivery_set"]["count"] == 1
    assert (value["complete_set_equality"] is value["zero_status"] is
            value["zero_loss_and_exclusion_counters"] is True)


def test_hook_callgraph_v2_requires_sut_and_extension_runtime_bindings():
    identity = _derive_identity(_exact_files())
    declaration = {"role": "sut", "name": "candidate", "source": "exe",
        "expected_path": "/offline/candidate", "sha256": "aa" * 32,
        "expected_device": 1, "expected_inode": 2, "expected_size": 3}
    resolution = {"schema": "mw_perf.p0a.resolution.v2", **IDENTITY,
        "declaration_sha256": hashlib.sha256(dds_trace._canonical_bytes(
            [declaration])).hexdigest(), "declarations": [declaration],
        "matches": [{"role": "sut", "name": "candidate", "source": "exe",
            "expected_path": "/offline/candidate", "expected_sha256": "aa" * 32,
            "expected_device": 1, "expected_inode": 2, "expected_size": 3,
            "observed": ["bb" * 32]}]}
    with pytest.raises(ValueError, match="resolved sut and extension"):
        derive_hook_callgraph(identity, resolution,
                              dds_trace._canonical_bytes(resolution, newline=True),
                              registration_contract_sha256="cc" * 32)


def test_normalized_v2_single_delivery_is_deterministic_closed_and_allows_overlap():
    stages = [_record(kind=1, start=10, end=35),
              _record(kind=2, start=20, end=30),
              _record(kind=3, start=40, end=70)]
    inputs = _normalized_inputs(_exact_files(
        stage_records=stages, exact_records=[_exact_record(registration=73)]))
    first = derive_normalized_results(*inputs, inputs[3]["bytes"])
    second = derive_normalized_results(*inputs, inputs[3]["bytes"])
    assert first == second
    comm = first["comm_events"]; latency = first["latency_samples"]
    assert comm["record_count"] == 4 and latency["record_count"] == 1
    assert [item["event_kind"] for item in comm["records"]] == [
        "publish", "receive", "dispatch", "callback"]
    assert comm["records"][0]["end_ns"] > comm["records"][1]["start_ns"]
    assert latency["records"][0]["latency_ns"] == 40
    assert latency["records"][0]["measurement"] == "publish_start_to_callback_start"
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v6")
    assert all(not core._schema_errors(item, contract["artifact_schemas"][item["schema"]])
               for item in comm["records"] + latency["records"])
    assert set(latency["records"][0]) == set(
        contract["artifact_schemas"]["mw_perf.latency_sample.v2"]["properties"])


def test_normalized_v2_fanout_has_one_publish_and_s_plus_three_d_formula():
    stages = [_record(kind=1, start=10, end=20)]
    for reader in ("55" * 16, "44" * 16):
        stages += [_record(kind=2, reader=reader, start=20, end=30),
                   _record(kind=3, reader=reader, start=40, end=70)]
    exact = [_exact_record(reader="55" * 16, registration=2),
             _exact_record(reader="44" * 16, registration=1)]
    inputs = _normalized_inputs(_exact_files(stage_records=stages, exact_records=exact))
    result = derive_normalized_results(*inputs, inputs[3]["bytes"])
    comm = result["comm_events"]["records"]
    assert len(comm) == 1 + 3 * 2
    assert sum(item["event_kind"] == "publish" for item in comm) == 1
    assert len(result["latency_samples"]["records"]) == 2
    assert [item["reader_guid"] for item in comm if item["event_kind"] == "callback"] == [
        "44" * 16, "55" * 16]


def test_normalized_v2_rejects_unrelated_hook_and_non_uint64_values():
    identity, resolution, resolution_bytes, hook = _normalized_inputs(_exact_files())
    unrelated_value = json.loads(json.dumps(hook["value"]))
    unrelated_value["inputs"]["identity_trace"]["content_sha256"] = "00" * 32
    unrelated_bytes = dds_trace._canonical_bytes(unrelated_value, newline=True)
    unrelated = {**hook, "value": unrelated_value, "bytes": unrelated_bytes,
                 "content_sha256": hashlib.sha256(unrelated_bytes).hexdigest()}
    with pytest.raises(ValueError, match="input binding"):
        derive_normalized_results(identity, resolution, resolution_bytes,
                                  unrelated, unrelated_bytes)
    changed = {**identity, "records": [dict(item) for item in identity["records"]]}
    changed["records"][0]["publish_start_ns"] = True
    changed["bytes"] = b"".join(dds_trace._canonical_bytes(item, newline=True)
                                 for item in changed["records"])
    changed["content_sha256"] = hashlib.sha256(changed["bytes"]).hexdigest()
    changed_hook = derive_hook_callgraph(changed, resolution, resolution_bytes,
                                         registration_contract_sha256="ee" * 32)
    with pytest.raises(ValueError, match="uint64"):
        derive_normalized_results(changed, resolution, resolution_bytes,
                                  changed_hook, changed_hook["bytes"])


@pytest.mark.parametrize("field,value,error", [
    ("publish_start_ns", -1, "uint64"),
    ("publish_end_ns", 1 << 64, "uint64"),
    ("receive_end_ns", 19, "reversed timestamps"),
    ("callback_registration_id", 0, "registration is malformed"),
    ("sequence_high", 1 << 31, "sequence is malformed"),
    ("callback_start_ns", 9, "latency is outside uint64"),
])
def test_normalized_v2_rejects_numeric_boundaries(field, value, error):
    identity, resolution, resolution_bytes, _hook = _normalized_inputs(_exact_files())
    changed = {**identity, "records": [dict(item) for item in identity["records"]]}
    changed["records"][0][field] = value
    changed["bytes"] = b"".join(dds_trace._canonical_bytes(item, newline=True)
                                 for item in changed["records"])
    changed["content_sha256"] = hashlib.sha256(changed["bytes"]).hexdigest()
    changed_hook = derive_hook_callgraph(changed, resolution, resolution_bytes,
                                         registration_contract_sha256="ee" * 32)
    with pytest.raises(ValueError, match=error):
        derive_normalized_results(changed, resolution, resolution_bytes,
                                  changed_hook, changed_hook["bytes"])


@pytest.mark.parametrize("fanout", [False, True])
def test_communication_latency_v2_is_deterministic_closed_and_matches_hook(fanout):
    stages = [_record(kind=1, start=10, end=20)]
    exact = [_exact_record(reader="44" * 16, registration=1)]
    stages += [_record(kind=2, reader="44" * 16, start=20, end=30),
               _record(kind=3, reader="44" * 16, start=40, end=70)]
    if fanout:
        stages += [_record(kind=2, reader="55" * 16, start=21, end=31),
                   _record(kind=3, reader="55" * 16, start=41, end=71)]
        exact.append(_exact_record(reader="55" * 16, registration=2,
                                   start=51, end=61))
    identity, resolution, resolution_bytes, hook = _normalized_inputs(
        _exact_files(stage_records=stages, exact_records=exact))
    normalized = derive_normalized_results(
        identity, resolution, resolution_bytes, hook, hook["bytes"])
    first = derive_communication_latency(
        identity, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        registration_contract_sha256="ee" * 32)
    second = derive_communication_latency(
        identity, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        registration_contract_sha256="ee" * 32)
    assert first == second and first["bytes"].endswith(b"\n")
    value = first["value"]
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v7")
    schema = contract["artifact_schemas"]["mw_perf.communication_latency.v2"]
    assert not core._schema_errors(value, schema)
    assert set(value) == set(schema["properties"])
    assert "stats" not in json.dumps(value)
    assert value["sample_set"]["sha256"] == hook["delivery_set_sha256"]
    assert value["sample_set"]["count"] == hook["value"]["delivery_set"]["count"]
    assert value["inputs"]["latency_samples"]["content_sha256"] == \
        hashlib.sha256(normalized["latency_samples"]["bytes"]).hexdigest()
    assert value["support"]["metric_availability"] == "unsupported"
    assert value["support"]["valid_for_comparison"] is False


def test_communication_latency_v2_rejects_tamper_and_sample_mismatch():
    identity, resolution, resolution_bytes, hook = _normalized_inputs(_exact_files())
    normalized = derive_normalized_results(
        identity, resolution, resolution_bytes, hook, hook["bytes"])
    changed = {**normalized, "latency_samples": dict(normalized["latency_samples"])}
    changed["latency_samples"]["bytes"] = normalized["latency_samples"]["bytes"].replace(
        b'"latency_ns":40', b'"latency_ns":41')
    with pytest.raises(ValueError, match="normalized bytes mismatch"):
        derive_communication_latency(identity, resolution, resolution_bytes, hook,
            hook["bytes"], changed, registration_contract_sha256="ee" * 32)
    wrong_hook = json.loads(json.dumps(hook["value"]))
    wrong_hook["delivery_set"]["sha256"] = "00" * 32
    wrong_bytes = dds_trace._canonical_bytes(wrong_hook, newline=True)
    wrong_derivation = {**hook, "value": wrong_hook, "bytes": wrong_bytes,
        "content_sha256": hashlib.sha256(wrong_bytes).hexdigest(),
        "delivery_set_sha256": "00" * 32}
    with pytest.raises(ValueError, match="mismatch"):
        derive_communication_latency(identity, resolution, resolution_bytes,
            wrong_derivation, wrong_bytes, normalized,
            registration_contract_sha256="ee" * 32)


def _delivery_inputs(count=1000):
    stages = []
    callbacks = []
    for sequence in range(1, count + 1):
        base = sequence * 100
        stages.extend([
            _record(sequence, kind=1, start=base, end=base + 10),
            _record(sequence, kind=2, start=base + 10, end=base + 20),
            _record(sequence, kind=3, start=base + 30, end=base + 60),
        ])
        callbacks.append(_exact_record(sequence=sequence, registration=1,
                                       start=base + 40, end=base + 50))
    identity, resolution, resolution_bytes, hook = _normalized_inputs(
        _exact_files(stage_records=stages, exact_records=callbacks))
    normalized = derive_normalized_results(
        identity, resolution, resolution_bytes, hook, hook["bytes"])
    latency = derive_communication_latency(
        identity, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        registration_contract_sha256="ee" * 32)
    return identity, resolution, resolution_bytes, hook, normalized, latency


DELIVERY_EXPECTATIONS = {"publish_count": 1000, "readers_per_sample": 1,
    "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records"}


def test_communication_delivery_v2_derives_1000_and_is_schema_closed():
    identity, resolution, resolution_bytes, hook, normalized, latency = \
        _delivery_inputs()
    derived = derive_communication_delivery(
        identity, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        latency, registration_contract_sha256="ee" * 32,
        expectations=DELIVERY_EXPECTATIONS)
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    schema = contract["artifact_schemas"]["mw_perf.communication_delivery.v2"]
    assert not core._schema_errors(derived["value"], schema)
    assert set(derived["value"]) == set(schema["properties"])
    assert derived["observed_publish_count"] == 1000
    assert derived["value"]["observed"]["delivery_set"]["sha256"] == \
        hook["delivery_set_sha256"]


def test_communication_delivery_rejects_order_permutation_count_and_fanout():
    identity, resolution, resolution_bytes, hook, normalized, latency = \
        _delivery_inputs()
    changed = {**identity, "trace_result": dict(identity["trace_result"])}
    stages = list(changed["trace_result"]["raw_stage_events"])
    stages[0], stages[3] = stages[3], stages[0]
    changed["trace_result"]["raw_stage_events"] = stages
    with pytest.raises(ValueError, match="order is not monotonic"):
        derive_communication_delivery(changed, resolution, resolution_bytes, hook,
            hook["bytes"], normalized, latency,
            registration_contract_sha256="ee" * 32,
            expectations=DELIVERY_EXPECTATIONS)

    changed = {**identity, "trace_result": dict(identity["trace_result"])}
    changed["trace_result"]["raw_callback_events"] = \
        list(changed["trace_result"]["raw_callback_events"][:-1])
    with pytest.raises(ValueError, match="event count mismatch"):
        derive_communication_delivery(changed, resolution, resolution_bytes, hook,
            hook["bytes"], normalized, latency,
            registration_contract_sha256="ee" * 32,
            expectations=DELIVERY_EXPECTATIONS)

    changed_expectations = dict(DELIVERY_EXPECTATIONS, readers_per_sample=2)
    with pytest.raises(ValueError, match="frozen v8 values"):
        derive_communication_delivery(identity, resolution, resolution_bytes, hook,
            hook["bytes"], normalized, latency,
            registration_contract_sha256="ee" * 32,
            expectations=changed_expectations)


def test_trace_quality_v1_is_canonical_closed_and_candidate_only():
    identity, resolution, resolution_bytes, hook, normalized, latency = _delivery_inputs()
    delivery = derive_communication_delivery(
        identity, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        latency, registration_contract_sha256="ee" * 32,
        expectations=DELIVERY_EXPECTATIONS)
    derived = derive_trace_quality(identity, delivery,
        registration_contract_version="mw_perf.p0a_contract.v8",
        registration_contract_sha256="ee" * 32)
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    schema = contract["artifact_schemas"]["mw_perf.trace_quality.v1"]
    assert not core._schema_errors(derived["value"], schema)
    assert derived["bytes"] == dds_trace._canonical_bytes(derived["value"], newline=True)
    assert derived["value"]["loss_accounting"] == {
        "sampling_exclusion_count": 0, "trace_loss_count": 0,
        "sampling_exclusion_distinct_from_trace_loss": True,
        "zero_loss_exclusion_status_pending_and_io": True}
    assert derived["value"]["support"] == {
        "level": "candidate_run_only", "formal_p0a_completion": False,
        "failure_code": "P0A_ARTIFACT"}


def test_trace_quality_rejects_stale_delivery_hash_and_nonzero_counter():
    identity, resolution, resolution_bytes, hook, normalized, latency = _delivery_inputs()
    delivery = derive_communication_delivery(
        identity, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        latency, registration_contract_sha256="ee" * 32,
        expectations=DELIVERY_EXPECTATIONS)
    stale = dict(delivery, content_sha256="00" * 32)
    with pytest.raises(ValueError, match="delivery artifact binding is stale"):
        derive_trace_quality(identity, stale,
            registration_contract_version="mw_perf.p0a_contract.v8",
            registration_contract_sha256="ee" * 32)
    stale = dict(delivery, record_count=2)
    with pytest.raises(ValueError, match="delivery artifact binding is stale"):
        derive_trace_quality(identity, stale,
            registration_contract_version="mw_perf.p0a_contract.v8",
            registration_contract_sha256="ee" * 32)

    changed = dict(identity)
    changed["trace_result"] = dict(identity["trace_result"])
    changed["trace_result"]["trace_quality_facts"] = dict(
        identity["trace_result"]["trace_quality_facts"])
    changed["trace_result"]["trace_quality_facts"]["stage"] = dict(
        identity["trace_result"]["trace_quality_facts"]["stage"], dropped_after=1)
    with pytest.raises(ValueError, match="loss, counter, or correlation"):
        derive_trace_quality(changed, delivery,
            registration_contract_version="mw_perf.p0a_contract.v8",
            registration_contract_sha256="ee" * 32)


def test_raw_stage_events_follow_segment_index_not_filename_or_mapping_order():
    segments = [[_record(1, kind=1, start=10, end=20),
                 _record(1, kind=2, start=20, end=30),
                 _record(1, kind=3, start=40, end=70)],
                [_record(2, kind=1, start=110, end=120),
                 _record(2, kind=2, start=120, end=130),
                 _record(2, kind=3, start=140, end=170)]]
    files = _exact_files(stage_segments=segments,
        exact_records=[_exact_record(sequence=1, start=50, end=60),
                       _exact_record(sequence=2, start=150, end=160)])
    reordered = dict(reversed(list(files.items())))
    result = _validate_exact(reordered)
    assert [item["sequence_low"] for item in result["raw_stage_events"]] == \
        [1, 1, 1, 2, 2, 2]
    assert [item["sequence_low"] for item in result["raw_callback_events"]] == [1, 2]


def test_identity_v2_rejects_multiple_stage_streams():
    files = _exact_files()
    stream = "aa" * 16
    meta = _meta(stream=stream)
    meta_raw = json.dumps(meta, separators=(",", ":")).encode()
    files["other.meta.json"] = meta_raw
    files["other.000000.ibct"] = _binary(meta_raw, records=[], stream=stream)
    seal = json.loads(files["stream.seal.json"])
    seal.update(stream_id=stream, segment_count=1, record_count=0, exported=0)
    files["other.seal.json"] = json.dumps(seal, separators=(",", ":")).encode()
    with pytest.raises(ValueError, match="exactly one matching stage stream"):
        _derive_identity(files)


def test_exact_correlation_rejects_single_handler_extra_stage_event():
    stages = [
        _record(kind=1, start=10, end=20),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=40, end=70),
        _record(sequence=2, kind=1, start=80, end=90),
    ]
    with pytest.raises(ValueError, match="match exact stream"):
        _validate_exact(_exact_files(stage_records=stages))


@pytest.mark.parametrize("stage_status,exact_status", [(1, 0), (0, 1)])
def test_exact_correlation_rejects_nonzero_status(stage_status, exact_status):
    stages = [
        _record(kind=1, status=stage_status, start=10, end=20),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=40, end=70),
    ]
    with pytest.raises(ValueError, match="zero status"):
        _validate_exact(_exact_files(stage_records=stages,
            exact_records=[_exact_record(status=exact_status)]))


@pytest.mark.parametrize("stages,exact", [
    ([
        _record(kind=1, start=21, end=25),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=40, end=70),
    ], _exact_record()),
    ([
        _record(kind=1, start=10, end=25),
        _record(kind=2, start=20, end=41),
        _record(kind=3, start=40, end=70),
    ], _exact_record()),
    ([
        _record(kind=1, start=10, end=25),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=51, end=70),
    ], _exact_record()),
    ([
        _record(kind=1, start=10, end=25),
        _record(kind=2, start=20, end=30),
        _record(kind=3, start=40, end=59),
    ], _exact_record()),
], ids=["publish-start", "receive-end", "dispatch-start", "dispatch-end"])
def test_exact_correlation_rejects_timestamp_inversion(stages, exact):
    with pytest.raises(ValueError, match="timestamp order"):
        _validate_exact(_exact_files(stage_records=stages, exact_records=[exact]))


def test_directory_scan_and_atomic_finalization_reject_unsafe_entries(tmp_path):
    trace = tmp_path / "trace"; trace.mkdir(mode=0o700)
    for name, data in _files().items(): (trace / name).write_bytes(data)
    fd = os.open(trace, os.O_RDONLY | os.O_DIRECTORY)
    try:
        files = scan_directory_fd(fd)
        raw = write_directory_finalization(fd, IDENTITY,
            [f"raw/comm_trace/{name}" for name in files])
        assert json.loads(raw)["finalized"] is True
        assert json.loads(raw)["files"] == sorted(json.loads(raw)["files"], key=lambda x: x.encode())
    finally: os.close(fd)
    for name, setup in (
        ("unknown", lambda: (trace / "unknown.txt").write_text("x")),
        ("part", lambda: (trace / "pending.part").write_text("x")),
        ("symlink", lambda: (trace / "link.ibct").symlink_to(trace / "stream.000000.ibct")),
        ("hardlink", lambda: os.link(trace / "stream.000000.ibct", trace / "hard.ibct")),
    ):
        setup(); fd = os.open(trace, os.O_RDONLY | os.O_DIRECTORY)
        try:
            with pytest.raises((ValueError, OSError)): scan_directory_fd(fd)
        finally: os.close(fd)
        for child in trace.iterdir():
            if child.name in ("unknown.txt", "pending.part", "link.ibct", "hard.ibct"):
                child.unlink()


def test_directory_scan_rejects_inode_substitution_between_stat_and_open(tmp_path, monkeypatch):
    trace = tmp_path / "trace"; trace.mkdir(mode=0o700)
    target = trace / "stream.meta.json"; target.write_bytes(b"old")
    replacement = tmp_path / "replacement"; replacement.write_bytes(b"new")
    fd = os.open(trace, os.O_RDONLY | os.O_DIRECTORY)
    real_open = os.open
    replaced = False
    def swapping_open(path, flags, *args, **kwargs):
        nonlocal replaced
        if path == "stream.meta.json" and not replaced:
            replaced = True
            os.replace(replacement, target)
        return real_open(path, flags, *args, **kwargs)
    monkeypatch.setattr(dds_trace.os, "open", swapping_open)
    try:
        with pytest.raises(OSError, match="changed before open"):
            scan_directory_fd(fd)
    finally:
        os.close(fd)


def test_finalization_confirmation_rejects_changes_between_scans(tmp_path):
    trace = tmp_path / "trace"; trace.mkdir(mode=0o700)
    for name, data in _files().items(): (trace / name).write_bytes(data)
    fd = os.open(trace, os.O_RDONLY | os.O_DIRECTORY)
    try:
        producer_files = scan_directory_fd(fd)
        (trace / "stream.seal.json").write_bytes(b"changed")
        with pytest.raises(OSError, match="changed after validation"):
            confirm_directory_unchanged(fd, producer_files)
        assert not (trace / "directory_finalization.json").exists()
    finally:
        os.close(fd)


def test_trusted_contract_loader_supports_v1_and_pattern_v2():
    v1, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v1")
    v2, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v2")
    assert v1["registration_contract_version"].endswith("v1")
    directory = next(item for item in v2["artifacts"]
                     if item["relative_path"] == "raw/comm_trace/")
    assert {item["pattern"] for item in directory["patterns"]} == {
        "*.meta.json", "*.ibct", "*.seal.json", "*.exact.meta.json",
        "*.ibce", "*.exact.seal.json"}


@pytest.mark.parametrize("exit_code, expect_supported", [(0, True), (7, False)])
def test_managed_p0a_requires_zero_exit_before_finalization(tmp_path, exit_code, expect_supported):
    script = r'''
import hashlib,json,os,signal,struct,time
fd=int(os.environ["MW_PERF_COMM_TRACE_DIR_FD"])
def ticks():
    raw=open("/proc/self/stat").read(); return int(raw[raw.rfind(")")+1:].split()[19])
def put(name,data):
    out=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=fd)
    try: os.write(out,data); os.fsync(out)
    finally: os.close(out)
def stop(*_):
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
    identity={"run_id":os.environ["MW_PERF_RUN_ID"],"capability_id":os.environ["MW_PERF_CAPABILITY_ID"],"path_id":os.environ["MW_PERF_PATH_ID"],"deployment_mode":os.environ["MW_PERF_DEPLOYMENT_MODE"]}
    stream="11"*16
    meta={"schema":"mw_perf.dds_comm_trace.meta.v1",**identity,"finalized":True,"stream_id":stream,"producer_pid":os.getpid(),"producer_start_time_ticks":ticks(),"topology_generation":os.environ["MW_PERF_TOPOLOGY_GENERATION"],"topology_sha256":os.environ["MW_PERF_TOPOLOGY_SHA256"],"trace_mode":os.environ["IBMW_COMM_TRACE"],"sampling_algorithm":0,"sampling_permyriad":10000}
    meta_raw=json.dumps(meta,separators=(",",":")).encode(); put("stream.meta.json",meta_raw)
    record=bytearray(64); struct.pack_into("<QQ",record,0,10,20); record[16:32]=bytes.fromhex("33"*16); struct.pack_into("<iI",record,32,0,1); struct.pack_into("<I",record,56,7); record[60:64]=bytes((1,0,1,0))
    header=bytearray(256); header[:8]=b"IBMWDCT\0"; struct.pack_into("<HHHHB",header,8,1,256,1,64,1); header[24:40]=bytes.fromhex(stream); header[40:72]=hashlib.sha256(meta_raw).digest(); struct.pack_into("<IIQQQQQQ",header,72,os.getpid(),0,ticks(),0,1,0,0,0); struct.pack_into("<Q",header,128,0); header[136:168]=hashlib.sha256(record).digest(); header[168]=2; struct.pack_into("<H",header,170,10000); put("stream.000000.ibct",bytes(header)+record)
    seal={"schema":"mw_perf.dds_comm_trace.seal.v1",**identity,"finalized":True,"stream_id":stream,"producer_pid":os.getpid(),"producer_start_time_ticks":ticks(),"trace_mode":"full","sampling_algorithm":0,"sampling_permyriad":10000,"segment_count":1,"record_count":1,"exported":1,"dropped":0,"invalid_identity":0,"unsupported_async":0,"export_discarded":0,"io_error":False,"pending":0,"active_producers":0,"sealed":True}
    put("stream.seal.json",json.dumps(seal,separators=(",",":")).encode()); os.fsync(fd); raise SystemExit(EXIT_CODE)
signal.signal(signal.SIGTERM,stop)
while True: time.sleep(.01)
'''.replace("EXIT_CODE", str(exit_code))
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "validation_profile": "p0a",
        "run": {"sample_seconds": .1, "stop_timeout_seconds": 1},
        "sut": {"mode": "managed", "command": [sys.executable, "-c", script],
                "env": {"MW_PERF_RUN_ID": "forged", "MW_PERF_CAPABILITY_ID": "forged",
                        "MW_PERF_PATH_ID": "forged", "MW_PERF_DEPLOYMENT_MODE": "forged",
                        "MW_PERF_COMM_TRACE_DIR_FD": "999",
                        "MW_PERF_TOPOLOGY_GENERATION": "forged",
                        "MW_PERF_TOPOLOGY_SHA256": "0" * 64,
                        "IBMW_COMM_TRACE": "sampled", "IBMW_COMM_TRACE_SINK": "none"}},
        "p0a_evidence": {"run_type": "stage_run", "observer": False, "trace_mode": "full",
                         "capability_id": "cap", "path_id": "path",
                         "topology_generation": "generation-1", "measurement_kind": "stage_only"},
        "scenario_contract": {"topology": "frozen"},
    }))
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(tmp_path / "runs"))
    assert core.run(args) == core.EXIT_INVALID  # Other stage_run artifacts are intentionally absent.
    run_dir = next((tmp_path / "runs").iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["dds_comm_trace"]["status"] == (
        "supported" if expect_supported else "invalid"), manifest
    if not expect_supported:
        assert manifest["run_status"] != "completed" or manifest["dds_comm_trace"]["status"] == "invalid"
        assert not (run_dir / "raw/comm_trace/directory_finalization.json").exists()
        return
    assert manifest["dds_comm_trace"]["record_count"] == 1
    assert manifest["dds_comm_trace"]["topology_generation"] == "generation-1"
    assert manifest["runtime_changed"] is True and manifest["business_unchanged"] is True
    finalization = json.loads((run_dir / "raw/comm_trace/directory_finalization.json").read_text())
    assert finalization["run_id"] == run_dir.name and finalization["finalized"] is True
    meta = json.loads((run_dir / "raw/comm_trace/stream.meta.json").read_text())
    assert manifest["dds_comm_trace"]["topology_sha256"] == meta["topology_sha256"]
    assert meta["run_id"] == run_dir.name and meta["trace_mode"] == "full"
    assert {key: meta[key] for key in ("capability_id", "path_id", "deployment_mode",
        "topology_generation")} == {"capability_id": "cap", "path_id": "path",
        "deployment_mode": "managed", "topology_generation": "generation-1"}
    assert meta["topology_sha256"] != "0" * 64
    open_targets = []
    for name in os.listdir("/proc/self/fd"):
        try: open_targets.append(os.readlink(f"/proc/self/fd/{name}"))
        except OSError: pass
    assert str(run_dir / "raw/comm_trace") not in open_targets


def test_managed_exact_run_publishes_and_offline_validates_v4_identity(tmp_path):
    script = r'''
import hashlib,json,os,signal,struct,time
fd=int(os.environ["MW_PERF_COMM_TRACE_DIR_FD"])
trace_root=os.readlink(f"/proc/self/fd/{fd}")
run_root=os.path.dirname(os.path.dirname(trace_root))
identity={"run_id":os.environ["MW_PERF_RUN_ID"],"capability_id":os.environ["MW_PERF_CAPABILITY_ID"],"path_id":os.environ["MW_PERF_PATH_ID"],"deployment_mode":os.environ["MW_PERF_DEPLOYMENT_MODE"]}
def ticks():
    raw=open("/proc/self/stat").read(); return int(raw[raw.rfind(")")+1:].split()[19])
start_ticks=ticks()
def trace_put(name,data):
    out=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=fd)
    try: os.write(out,data); os.fsync(out)
    finally: os.close(out)
def artifact(relative,schema,**fields):
    value={"schema":schema,**identity,"finalized":True,**fields}
    target=os.path.join(run_root,relative)
    with open(target,"x") as out:
        json.dump(value,out,separators=(",",":")); out.write("\n" if relative.endswith(".jsonl") else ""); out.flush(); os.fsync(out.fileno())
def stop(*_):
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
    artifact("capability/hook_callgraph.json","mw_perf.p0a.hook_callgraph.v1",hooks_verified=True,conclusion="pass")
    artifact("validation/fault_injection.json","mw_perf.p0a.fault_injection.v1",fault_detected=True,conclusion="pass")
    artifact("validation/rollback.json","mw_perf.p0a.rollback.v1",rollback_succeeded=True,health_check_passed=True,conclusion="pass")
    artifact("normalized/comm_events.jsonl","mw_perf.comm_event.v1")
    artifact("normalized/latency_samples.jsonl","mw_perf.latency_sample.v1")
    artifact("capability/communication_latency.json","mw_perf.communication_latency.v1",conclusion="pass")
    artifact("validation/communication_delivery.json","mw_perf.communication_delivery.v1",conclusion="pass")
    stream="11"*16
    controls={"producer_pid":os.getpid(),"producer_start_time_ticks":start_ticks,"topology_generation":os.environ["MW_PERF_TOPOLOGY_GENERATION"],"topology_sha256":os.environ["MW_PERF_TOPOLOGY_SHA256"],"trace_mode":os.environ["IBMW_COMM_TRACE"]}
    meta={"schema":"mw_perf.dds_comm_trace.meta.v1",**identity,"finalized":True,"stream_id":stream,**controls,"sampling_algorithm":0,"sampling_permyriad":10000}
    meta_raw=json.dumps(meta,separators=(",",":")).encode(); trace_put("stream.meta.json",meta_raw)
    records=[]
    for kind,start,end in ((1,10,20),(2,20,30),(3,40,70)):
        record=bytearray(64); struct.pack_into("<QQ",record,0,start,end); record[16:32]=bytes.fromhex("33"*16); struct.pack_into("<iI",record,32,0,1); record[40:56]=bytes.fromhex(("00" if kind==1 else "44")*16); struct.pack_into("<I",record,56,7); record[60:64]=bytes((kind,0,1,0)); records.append(record)
    payload=b"".join(records)
    header=bytearray(256); header[:8]=b"IBMWDCT\0"; struct.pack_into("<HHHHB",header,8,1,256,1,64,1); header[24:40]=bytes.fromhex(stream); header[40:72]=hashlib.sha256(meta_raw).digest(); struct.pack_into("<IIQQQQQQ",header,72,os.getpid(),0,start_ticks,0,3,0,0,0); struct.pack_into("<Q",header,128,0); header[136:168]=hashlib.sha256(payload).digest(); header[168]=2; struct.pack_into("<H",header,170,10000); trace_put("stream.000000.ibct",bytes(header)+payload)
    seal={"schema":"mw_perf.dds_comm_trace.seal.v1",**identity,"finalized":True,"stream_id":stream,"producer_pid":os.getpid(),"producer_start_time_ticks":start_ticks,"trace_mode":"full","sampling_algorithm":0,"sampling_permyriad":10000,"segment_count":1,"record_count":3,"exported":3,"dropped":0,"invalid_identity":0,"unsupported_async":0,"export_discarded":0,"io_error":False,"pending":0,"active_producers":0,"sealed":True}
    trace_put("stream.seal.json",json.dumps(seal,separators=(",",":")).encode())
    exact_stream=stream
    exact_meta={"schema":"mw_perf.dds_exact_callback.meta.v1",**identity,"finalized":True,"stream_id":exact_stream,**controls,"record_size":72}
    exact_meta_raw=json.dumps(exact_meta,separators=(",",":")).encode(); trace_put("trace.exact.meta.json",exact_meta_raw)
    exact_record=bytearray(72); struct.pack_into("<QQ",exact_record,0,50,60); exact_record[16:32]=bytes.fromhex("33"*16); struct.pack_into("<iI",exact_record,32,0,1); exact_record[40:56]=bytes.fromhex("44"*16); struct.pack_into("<Q",exact_record,56,73); exact_record[64:67]=bytes((1,0,1))
    exact_header=bytearray(160); exact_header[:8]=b"IBMWDCE\0"; struct.pack_into("<HHHB",exact_header,8,1,160,72,1); exact_header[16:32]=bytes.fromhex(exact_stream); exact_header[32:64]=hashlib.sha256(exact_meta_raw).digest(); struct.pack_into("<I",exact_header,64,os.getpid()); struct.pack_into("<Q",exact_header,72,start_ticks); struct.pack_into("<Q",exact_header,80,1); exact_header[128:160]=hashlib.sha256(exact_record).digest(); trace_put("trace.ibce",bytes(exact_header)+exact_record)
    exact_seal={"schema":"mw_perf.dds_exact_callback.seal.v1",**identity,"finalized":True,"stream_id":exact_stream,"producer_pid":os.getpid(),"producer_start_time_ticks":start_ticks,"trace_mode":"full","record_count":1,"dropped":0,"invalid_identity":0,"missing_registration":0,"unsupported_async":0,"unsupported_multi_handler":0,"pending":0,"io_error":False,"sealed":True}
    trace_put("trace.exact.seal.json",json.dumps(exact_seal,separators=(",",":")).encode()); os.fsync(fd)
    raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
while True: time.sleep(.01)
'''
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "validation_profile": "p0a",
        "run": {"sample_seconds": .05, "stop_timeout_seconds": 1},
        "sut": {"mode": "managed", "command": [sys.executable, "-c", script]},
        "p0a_evidence": {
            "run_type": "exact_run", "trace_mode": "full",
            "registration_contract_version": "mw_perf.p0a_contract.v4",
            "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
            "path_id": "path", "topology_generation": "generation-1",
            "measurement_kind": "exact_correlated", "observer": False,
            "resolution": {"runtime_images": [{
                "role": "sut", "name": "python", "source": "exe",
                "expected_path": str(Path(sys.executable).resolve()),
                "sha256": hashlib.sha256(Path(sys.executable).resolve().read_bytes()).hexdigest(),
            }]},
        },
        "scenario_contract": {"topology": "frozen"},
    }))
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(tmp_path / "runs"))

    assert core.run(args) == core.EXIT_OK
    run_dir = next((tmp_path / "runs").iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["run_status"] == "completed"
    assert manifest["measurement_kind"] == "exact_correlated"
    trace = manifest["dds_comm_trace"]
    assert trace["status"] == "exact_correlated"
    assert trace["record_count"] == 3
    assert trace["exact_record_count"] == 1
    assert {trace[key] for key in ("dropped", "invalid_identity", "missing_registration",
        "unsupported_async", "unsupported_multi_handler")} == {0}
    assert (run_dir / "raw/comm_trace/directory_finalization.json").is_file()
    evidence = json.loads((run_dir / "capability/p0a_evidence_manifest.json").read_text())
    assert evidence["registration_contract_version"] == "mw_perf.p0a_contract.v4"
    observer = next(item for item in evidence["entries"]
                    if item["relative_path"] == "capability/observer_isolation.json")
    assert observer["status"] == "not_applicable"
    assert observer["finalization"] == "not_applicable"
    identity_path = run_dir / "normalized/identity_trace.jsonl"
    identity_raw = identity_path.read_bytes()
    identity = json.loads(identity_raw)
    assert identity["schema"] == "mw_perf.p0a.identity_trace.v2"
    assert identity["callback_registration_id"] == 73
    assert manifest["p0a_identity_trace"] == {
        "collector_written": True,
        "relative_path": "normalized/identity_trace.jsonl",
        "content_sha256": hashlib.sha256(identity_raw).hexdigest(),
        "source_set_sha256": identity["source_set_sha256"],
        "record_count": 1,
    }
    validation = core.validate_dir(run_dir, profile="p0a")
    assert validation["valid_for_comparison"], validation

    original_root = json.loads((run_dir / "manifest.json").read_text())
    original_record = json.loads(identity_raw)
    mutations = {
        "run_identity": lambda value: value.update(run_id="other"),
        "stream_identity": lambda value: value.update(stream_id="aa" * 16),
        "producer_pid": lambda value: value.update(producer_pid=value["producer_pid"] + 1),
        "producer_ticks": lambda value: value.update(
            producer_start_time_ticks=value["producer_start_time_ticks"] + 1),
        "topology_generation": lambda value: value.update(topology_generation="other"),
        "topology_sha256": lambda value: value.update(topology_sha256="aa" * 32),
        "delivery_identity": lambda value: value.update(writer_guid="55" * 16),
        "callback_registration": lambda value: value.update(callback_registration_id=2),
        "timestamp_order": lambda value: value.update(callback_start_ns=39),
        "source_hash": lambda value: value.update(source_set_sha256="aa" * 32),
    }
    for name, mutate in mutations.items():
        changed_record = dict(original_record); mutate(changed_record)
        changed_raw = json.dumps(changed_record, sort_keys=True, separators=(",", ":")).encode() + b"\n"
        identity_path.write_bytes(changed_raw)
        changed_evidence = json.loads(json.dumps(evidence))
        digest = hashlib.sha256(changed_raw).hexdigest()
        for group in (changed_evidence["entries"], changed_evidence["required"]):
            entry = next(item for item in group
                         if item["relative_path"] == "normalized/identity_trace.jsonl")
            entry.update(sha256=digest, record_count=1)
        changed_root = json.loads(json.dumps(original_root))
        changed_root["p0a_identity_trace"].update(
            content_sha256=digest, source_set_sha256=changed_record["source_set_sha256"])
        (run_dir / "manifest.json").write_text(json.dumps(changed_root))
        rejected = core.validate_p0a_evidence_manifest(run_dir, changed_evidence,
            producer_pid=manifest["pid"],
            producer_start_time_ticks=manifest["pid_start_time_ticks"],
            topology_generation=trace["topology_generation"],
            topology_sha256=trace["topology_sha256"])
        assert "P0A_IDENTITY_TRACE" in rejected["failure_codes"], name
    for name, changed_raw in {
            "duplicate_line": identity_raw + identity_raw,
            "missing_line": b"",
            "extra_line": identity_raw + json.dumps(
                {**original_record, "reader_guid": "55" * 16}, sort_keys=True,
                separators=(",", ":")).encode() + b"\n",
            }.items():
        identity_path.write_bytes(changed_raw)
        changed_evidence = json.loads(json.dumps(evidence))
        digest = hashlib.sha256(changed_raw).hexdigest()
        count = len(changed_raw.splitlines())
        for group in (changed_evidence["entries"], changed_evidence["required"]):
            entry = next(item for item in group
                         if item["relative_path"] == "normalized/identity_trace.jsonl")
            entry.update(sha256=digest, record_count=count)
        changed_root = json.loads(json.dumps(original_root))
        changed_root["p0a_identity_trace"].update(content_sha256=digest, record_count=count)
        (run_dir / "manifest.json").write_text(json.dumps(changed_root))
        rejected = core.validate_p0a_evidence_manifest(run_dir, changed_evidence,
            producer_pid=manifest["pid"],
            producer_start_time_ticks=manifest["pid_start_time_ticks"],
            topology_generation=trace["topology_generation"],
            topology_sha256=trace["topology_sha256"])
        assert {"P0A_ARTIFACT", "P0A_IDENTITY_TRACE"} & set(rejected["failure_codes"]), name
    identity_path.write_bytes(identity_raw)
    (run_dir / "manifest.json").write_text(json.dumps(original_root))
    segment_path = run_dir / "raw/comm_trace/stream.000000.ibct"
    segment_raw = segment_path.read_bytes()
    changed_segment = bytearray(segment_raw); changed_segment[-1] ^= 1
    segment_path.write_bytes(changed_segment)
    changed_record = dict(original_record); changed_record["source_set_sha256"] = "aa" * 32
    changed_identity = json.dumps(changed_record, sort_keys=True,
                                  separators=(",", ":")).encode() + b"\n"
    identity_path.write_bytes(changed_identity)
    changed_evidence = json.loads(json.dumps(evidence))
    updates = {
        "raw/comm_trace/stream.000000.ibct": hashlib.sha256(changed_segment).hexdigest(),
        "normalized/identity_trace.jsonl": hashlib.sha256(changed_identity).hexdigest(),
    }
    for group in (changed_evidence["entries"], changed_evidence["required"]):
        for relative, digest in updates.items():
            next(item for item in group if item["relative_path"] == relative)["sha256"] = digest
    changed_root = json.loads(json.dumps(original_root))
    changed_root["p0a_identity_trace"].update(
        content_sha256=updates["normalized/identity_trace.jsonl"],
        source_set_sha256=changed_record["source_set_sha256"])
    (run_dir / "manifest.json").write_text(json.dumps(changed_root))
    rejected = core.validate_p0a_evidence_manifest(run_dir, changed_evidence,
        producer_pid=manifest["pid"], producer_start_time_ticks=manifest["pid_start_time_ticks"],
        topology_generation=trace["topology_generation"], topology_sha256=trace["topology_sha256"])
    assert {"P0A_ARTIFACT", "P0A_DDS_COMM_TRACE"} & set(rejected["failure_codes"])
    assert "P0A_IDENTITY_TRACE" in rejected["failure_codes"]


def test_attached_p0a_binary_trace_is_required_stop_unsupported(tmp_path):
    config = tmp_path / "config.json"
    config.write_text(json.dumps({"validation_profile": "p0a",
        "sut": {"mode": "attached", "pid": os.getpid()},
        "p0a_evidence": {"run_type": "stage_run", "observer": False,
                         "measurement_kind": "stage_only", "trace_mode": "full"}}))
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(tmp_path / "runs"))
    assert core.run(args) != core.EXIT_OK
    run_dir = next((tmp_path / "runs").iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["run_status"] == "invalid" and manifest["required_stop"] is True
    assert manifest["dds_comm_trace"]["status"] == "unsupported"
    assert not (run_dir / "raw/comm_trace/directory_finalization.json").exists()


def test_sigkill_does_not_generate_directory_finalization(tmp_path):
    config = tmp_path / "config.json"
    config.write_text(json.dumps({"validation_profile": "p0a",
        "run": {"sample_seconds": .01, "stop_timeout_seconds": .01},
        "sut": {"mode": "managed", "command": [sys.executable, "-c",
            "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(10)"]},
        "p0a_evidence": {"run_type": "stage_run", "observer": False, "trace_mode": "full",
                         "measurement_kind": "stage_only"}}))
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(tmp_path / "runs"))
    assert core.run(args) != core.EXIT_OK
    run_dir = next((tmp_path / "runs").iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["stop_escalated_sigkill"] is True
    assert manifest["dds_comm_trace"]["status"] == "invalid"
    assert not (run_dir / "raw/comm_trace/directory_finalization.json").exists()
