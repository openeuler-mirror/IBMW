"""Validation and collector-side finalization for managed DDS comm traces."""
from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import struct
from pathlib import Path
from typing import Any

from .json_strict import loads as strict_json_loads

HEADER_SIZE = 256
RECORD_SIZE = 64
MAGIC = b"IBMWDCT\0"
HEADER_MAJOR = 1
EVENT_SCHEMA = 1
ENDIAN_LITTLE = 1
MODE_NAMES = {1: "sampled", 2: "full"}
KIND_NAMES = {1: "publish", 2: "receive", 3: "dispatch"}
ZERO_GUID = b"\0" * 16

HOOK_BOUNDARY_CATALOG = [
    {"logical_boundary": "publish_write",
     "production_function": "DdsTransportDriver::SendNonPlainType",
     "source_path": "src/extensions/transport/dds_ext/dds_transport_driver.cc",
     "source_binding": "trusted_contract_declaration"},
    {"logical_boundary": "take_receive",
     "production_function": "DdsTransportDriver::DdsDataReaderListener::HandleNonPlainType",
     "source_path": "src/extensions/transport/dds_ext/dds_transport_driver.cc",
     "source_binding": "trusted_contract_declaration"},
    {"logical_boundary": "sync_dispatch",
     "production_function": "MessageHandlerTool::DoMessageCallbackWithInvoker",
     "source_path": "src/core/engine/transport/transport_driver_tools.h",
     "source_binding": "trusted_contract_declaration"},
    {"logical_boundary": "final_callback_invocation",
     "production_function": "InvokeFinalMessageCallback",
     "source_path": "src/core/engine/transport/transport_driver_dispatcher_ops.cc",
     "source_binding": "trusted_contract_declaration"},
]


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def parse_binary(data: bytes) -> dict[str, Any]:
    """Parse one complete .ibct snapshot and return validated header/events."""
    if len(data) < HEADER_SIZE:
        raise ValueError("DDS trace is shorter than its header")
    header, payload = data[:HEADER_SIZE], data[HEADER_SIZE:]
    if header[:8] != MAGIC:
        raise ValueError("DDS trace magic mismatch")
    if (_u16(header, 8), _u16(header, 10), _u16(header, 12), _u16(header, 14), header[16]) != (
            HEADER_MAJOR, HEADER_SIZE, EVENT_SCHEMA, RECORD_SIZE, ENDIAN_LITTLE):
        raise ValueError("unsupported DDS trace format")
    if any(header[17:24]) or any(header[172:]):
        raise ValueError("nonzero reserved header bytes")
    mode = MODE_NAMES.get(header[168])
    sampling_algorithm, sampling_permyriad = header[169], _u16(header, 170)
    if mode is None or sampling_permyriad > 10000:
        raise ValueError("invalid DDS trace mode or sampling")
    if mode == "full" and (sampling_algorithm != 0 or sampling_permyriad != 10000):
        raise ValueError("full trace has sampled-mode settings")
    if mode == "sampled" and (sampling_algorithm != 1 or sampling_permyriad != 625):
        raise ValueError("sampled trace has invalid sampling settings")
    if len(payload) % RECORD_SIZE:
        raise ValueError("partial DDS trace record")
    count = len(payload) // RECORD_SIZE
    if count != _u64(header, 96):
        raise ValueError("DDS trace declared count mismatch")
    if hashlib.sha256(payload).digest() != header[136:168]:
        raise ValueError("DDS trace payload hash mismatch")
    events: list[dict[str, Any]] = []
    stages: set[tuple[bytes, int, int, int, bytes]] = set()
    for index in range(count):
        record = payload[index * RECORD_SIZE:(index + 1) * RECORD_SIZE]
        start_ns, end_ns = _u64(record, 0), _u64(record, 8)
        writer, reader = record[16:32], record[40:56]
        sequence_high = struct.unpack_from("<i", record, 32)[0]
        sequence_low = _u32(record, 36)
        kind, status, schema, reserved = record[60], record[61], record[62], record[63]
        if schema != EVENT_SCHEMA or kind not in KIND_NAMES or status not in (0, 1) or reserved != 0:
            raise ValueError(f"invalid DDS event control fields at {index}")
        if end_ns < start_ns or writer == ZERO_GUID or (sequence_high == -1 and sequence_low == 0):
            raise ValueError(f"invalid DDS event identity/timestamps at {index}")
        if (kind == 1 and reader != ZERO_GUID) or (kind in (2, 3) and reader == ZERO_GUID):
            raise ValueError(f"invalid reader GUID for DDS event kind at {index}")
        stage = (writer, sequence_high, sequence_low, kind, reader)
        if stage in stages:
            raise ValueError(f"duplicate SampleKey stage at {index}")
        stages.add(stage)
        events.append({"writer_guid": writer.hex(), "sequence_high": sequence_high,
                       "sequence_low": sequence_low, "reader_guid": reader.hex(),
                       "kind": KIND_NAMES[kind], "status": status,
                       "start_ns": start_ns, "end_ns": end_ns})
    return {
        "record_count": count,
        "events": events,
        "stream_id": header[24:40].hex(),
        "meta_sha256": header[40:72].hex(),
        "producer_pid": _u32(header, 72),
        "segment_index": _u32(header, 76),
        "producer_start_time_ticks": _u64(header, 80),
        "first_ordinal": _u64(header, 88),
        "dropped_before": _u64(header, 104),
        "dropped_after": _u64(header, 112),
        "invalid_identity": _u64(header, 120),
        "unsupported_async": _u64(header, 128),
        "mode": mode,
        "sampling_algorithm": sampling_algorithm,
        "sampling_permyriad": sampling_permyriad,
    }


def binary_record_count(data: bytes) -> int:
    return int(parse_binary(data)["record_count"])


def _json_object(data: bytes) -> dict[str, Any]:
    value = strict_json_loads(data)
    if not isinstance(value, dict):
        raise ValueError("DDS trace JSON must be an object")
    return value


def _identity(record: dict[str, Any], expected: dict[str, str]) -> None:
    for field, value in expected.items():
        if record.get(field) != value:
            raise ValueError(f"DDS trace identity mismatch: {field}")


def derive_exact_deliveries(stage_events: list[dict[str, Any]],
                            exact_events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Purely join full stage and callback streams by authoritative DDS identities."""
    if not exact_events:
        raise ValueError("exact correlation requires at least one exact delivery")
    if any(event["status"] != 0 for event in stage_events + exact_events):
        raise ValueError("exact correlation requires zero status for every event")

    def sample_key(event: dict[str, Any]) -> tuple[str, int, int]:
        return (event["writer_guid"], event["sequence_high"], event["sequence_low"])

    def delivery_key(event: dict[str, Any]) -> tuple[str, int, int, str]:
        return (*sample_key(event), event["reader_guid"])

    publishes: dict[tuple[str, int, int], dict[str, Any]] = {}
    receives: dict[tuple[str, int, int, str], dict[str, Any]] = {}
    dispatches: dict[tuple[str, int, int, str], dict[str, Any]] = {}
    stage_maps = {"publish": publishes, "receive": receives, "dispatch": dispatches}
    for event in stage_events:
        key = sample_key(event) if event["kind"] == "publish" else delivery_key(event)
        target = stage_maps[event["kind"]]
        if key in target:
            raise ValueError("duplicate logical key in exact stage stream")
        target[key] = event

    exact_by_delivery: dict[tuple[str, int, int, str], dict[str, Any]] = {}
    for event in exact_events:
        key = delivery_key(event)
        if key in exact_by_delivery:
            raise ValueError("duplicate exact DeliveryKey")
        exact_by_delivery[key] = event

    exact_samples = {key[:3] for key in exact_by_delivery}
    exact_deliveries = set(exact_by_delivery)
    if set(publishes) != exact_samples:
        raise ValueError("stage publish SampleKey set does not match exact stream")
    if set(receives) != exact_deliveries or set(dispatches) != exact_deliveries:
        raise ValueError("stage receive/dispatch DeliveryKey sets do not match exact stream")
    if len(stage_events) != len(publishes) + 2 * len(exact_deliveries):
        raise ValueError("exact correlation event count mismatch")

    joined: list[dict[str, Any]] = []
    for key, exact in exact_by_delivery.items():
        publish = publishes[key[:3]]
        receive = receives[key]
        dispatch = dispatches[key]
        if not (publish["start_ns"] <= receive["start_ns"]
                and receive["end_ns"] <= dispatch["start_ns"]
                <= exact["start_ns"] <= exact["end_ns"]
                <= dispatch["end_ns"]):
            raise ValueError("exact correlation timestamp order violation")
        joined.append({
            "writer_guid": key[0], "sequence_high": key[1], "sequence_low": key[2],
            "reader_guid": key[3],
            "callback_registration_id": exact["callback_registration_id"],
            "publish_start_ns": publish["start_ns"], "publish_end_ns": publish["end_ns"],
            "receive_start_ns": receive["start_ns"], "receive_end_ns": receive["end_ns"],
            "dispatch_start_ns": dispatch["start_ns"], "dispatch_end_ns": dispatch["end_ns"],
            "callback_start_ns": exact["start_ns"], "callback_end_ns": exact["end_ns"],
        })
    return sorted(joined, key=lambda item: (
        bytes.fromhex(item["writer_guid"]), item["sequence_high"], item["sequence_low"],
        bytes.fromhex(item["reader_guid"])))


def validate_directory_files(files: dict[str, bytes], schemas: dict[str, dict[str, Any]],
                             schema_errors: Any, expected: dict[str, str],
                             *, run_type: str, measurement_kind: str,
                             producer_pid: int | None = None,
                             producer_start_time_ticks: int | None = None,
                             topology_generation: str | None = None,
                             topology_sha256: str | None = None,
                             registration_contract_version: str | None = None) -> dict[str, Any]:
    """Validate producer files after a graceful managed shutdown."""
    meta_files = sorted(name for name in files if name.endswith(".meta.json")
                        and not name.endswith(".exact.meta.json"))
    binary_files = sorted(name for name in files if name.endswith(".ibct"))
    seal_files = sorted(name for name in files if name.endswith(".seal.json")
                        and not name.endswith(".exact.seal.json"))
    if not meta_files or not binary_files or not seal_files:
        raise ValueError("DDS trace requires meta, binary segments, and seal")
    metas: dict[str, tuple[dict[str, Any], str]] = {}
    for name in meta_files:
        record = _json_object(files[name])
        if schema_errors(record, schemas["mw_perf.dds_comm_trace.meta.v1"]):
            raise ValueError(f"invalid DDS trace meta schema: {name}")
        _identity(record, expected)
        controls = {"producer_pid": producer_pid,
                    "producer_start_time_ticks": producer_start_time_ticks,
                    "topology_generation": topology_generation,
                    "topology_sha256": topology_sha256}
        if any(value is not None and record.get(key) != value for key, value in controls.items()):
            raise ValueError("DDS trace meta does not match collector controls")
        stream_id = record["stream_id"]
        expected_sampling = ((0, 10000) if record["trace_mode"] == "full" else (1, 625))
        if (record["sampling_algorithm"], record["sampling_permyriad"]) != expected_sampling:
            raise ValueError("DDS trace meta has invalid sampling settings")
        if stream_id in metas:
            raise ValueError("duplicate DDS trace stream meta")
        metas[stream_id] = (record, hashlib.sha256(files[name]).hexdigest())
    segments: dict[str, list[tuple[str, dict[str, Any]]]] = {}
    all_stages: set[tuple[Any, ...]] = set()
    for name in binary_files:
        parsed = parse_binary(files[name])
        stream_id = parsed["stream_id"]
        if stream_id not in metas:
            raise ValueError("DDS trace segment has no matching meta")
        meta, meta_sha = metas[stream_id]
        if parsed["meta_sha256"] != meta_sha:
            raise ValueError("DDS trace meta hash mismatch")
        for left, right in (("producer_pid", "producer_pid"),
                            ("producer_start_time_ticks", "producer_start_time_ticks"),
                            ("mode", "trace_mode"),
                            ("sampling_algorithm", "sampling_algorithm"),
                            ("sampling_permyriad", "sampling_permyriad")):
            if parsed[left] != meta[right]:
                raise ValueError(f"DDS trace segment/meta mismatch: {left}")
        for event in parsed["events"]:
            stage = (event["writer_guid"], event["sequence_high"], event["sequence_low"],
                     event["kind"], event["reader_guid"])
            if stage in all_stages:
                raise ValueError("duplicate SampleKey stage across segments")
            all_stages.add(stage)
        segments.setdefault(stream_id, []).append((name, parsed))
    total_records = 0
    total_dropped_before = total_dropped = total_invalid = total_unsupported = 0
    stage_events_by_stream: dict[str, list[dict[str, Any]]] = {}
    for stream_id in sorted(segments, key=lambda item: bytes.fromhex(item)):
        items = segments[stream_id]
        items.sort(key=lambda item: item[1]["segment_index"])
        if items[0][1]["first_ordinal"] != 0:
            raise ValueError("DDS trace first segment ordinal is not zero")
        ordinal = 0
        prior_dropped = items[0][1]["dropped_before"]
        prior_invalid = prior_unsupported = 0
        for expected_index, (_name, item) in enumerate(items):
            if item["segment_index"] != expected_index or item["first_ordinal"] != ordinal:
                raise ValueError("DDS trace segment index/ordinal discontinuity")
            if (item["dropped_before"] != prior_dropped
                    or item["dropped_after"] < item["dropped_before"]
                    or item["invalid_identity"] < prior_invalid
                    or item["unsupported_async"] < prior_unsupported):
                raise ValueError("DDS trace segment counters are discontinuous")
            ordinal += item["record_count"]
            prior_dropped = item["dropped_after"]
            prior_invalid = item["invalid_identity"]
            prior_unsupported = item["unsupported_async"]
            total_records += item["record_count"]
            stage_events_by_stream.setdefault(stream_id, []).extend(item["events"])
        last = items[-1][1]
        total_dropped_before += items[0][1]["dropped_before"]
        total_dropped += last["dropped_after"]
        total_invalid += last["invalid_identity"]
        total_unsupported += last["unsupported_async"]
    seals: dict[str, dict[str, Any]] = {}
    total_exported = total_export_discarded = total_pending = total_active_producers = 0
    any_io_error = False
    for name in seal_files:
        seal = _json_object(files[name])
        if schema_errors(seal, schemas["mw_perf.dds_comm_trace.seal.v1"]):
            raise ValueError(f"invalid DDS trace seal schema: {name}")
        _identity(seal, expected)
        stream_id = seal["stream_id"]
        if stream_id in seals or stream_id not in segments:
            raise ValueError("duplicate or unknown DDS trace seal")
        meta = metas[stream_id][0]
        items = segments[stream_id]
        exported = sum(item[1]["record_count"] for item in items)
        last = items[-1][1]
        checks = {
            "producer_pid": last["producer_pid"], "producer_start_time_ticks": last["producer_start_time_ticks"],
            "trace_mode": last["mode"], "sampling_algorithm": last["sampling_algorithm"],
            "sampling_permyriad": last["sampling_permyriad"], "segment_count": len(items),
            "record_count": exported, "exported": exported, "dropped": last["dropped_after"],
            "invalid_identity": last["invalid_identity"], "unsupported_async": last["unsupported_async"],
        }
        if any(seal.get(key) != value for key, value in checks.items()):
            raise ValueError("DDS trace seal/header count or identity mismatch")
        if any(seal.get(key) != meta.get(key) for key in (
                "producer_pid", "producer_start_time_ticks", "trace_mode",
                "sampling_algorithm", "sampling_permyriad")):
            raise ValueError("DDS trace seal/meta mismatch")
        if (seal.get("finalized") is not True or seal.get("sealed") is not True
                or seal.get("io_error") is not False or seal.get("pending") != 0
                or seal.get("active_producers") != 0 or seal.get("export_discarded") != 0
                or seal.get("dropped") != 0 or seal.get("invalid_identity") != 0
                or seal.get("unsupported_async") != 0):
            raise ValueError("DDS trace required zero-loss finalization failed")
        seals[stream_id] = seal
        total_exported += seal["exported"]
        total_export_discarded += seal["export_discarded"]
        total_pending += seal["pending"]
        total_active_producers += seal["active_producers"]
        any_io_error = any_io_error or seal["io_error"]
    if set(seals) != set(segments) or set(metas) != set(segments):
        raise ValueError("DDS trace stream meta/segment/seal set mismatch")
    result = {"status": "supported", "runtime_changed": True, "business_unchanged": True,
              "record_count": total_records, "stream_count": len(segments),
              "segment_count": len(binary_files), "dropped": total_dropped,
              "invalid_identity": total_invalid, "unsupported_async": total_unsupported}
    if run_type == "exact_run":
        from .exact_trace import validate_managed_exact
        exact_result = validate_managed_exact(files, schemas, schema_errors, expected,
            producer_pid=producer_pid, producer_start_time_ticks=producer_start_time_ticks,
            topology_generation=topology_generation, topology_sha256=topology_sha256)
        exact_stream_id = exact_result.pop("stream_id")
        if exact_stream_id not in segments:
            raise ValueError("exact stream identity does not match a stage stream")
        if (registration_contract_version in (
                "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5",
                "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
                "mw_perf.p0a_contract.v8", "mw_perf.p0a_ohos_remote_contract.v1")
                and (len(segments) != 1 or set(segments) != {exact_stream_id})):
            raise ValueError("contract v4-v8 requires exactly one matching stage stream")
        stage_events = stage_events_by_stream[exact_stream_id]
        callback_events = list(exact_result["events"])
        joined = derive_exact_deliveries(stage_events, callback_events)
        exact_seal = next(_json_object(files[name]) for name in files
                          if name.endswith(".exact.seal.json"))
        result["trace_quality_facts"] = {
            "trace_mode": next(iter(metas.values()))[0]["trace_mode"],
            "sampling_algorithm": next(iter(metas.values()))[0]["sampling_algorithm"],
            "sampling_permyriad": next(iter(metas.values()))[0]["sampling_permyriad"],
            "topology_generation": next(iter(metas.values()))[0]["topology_generation"],
            "topology_sha256": next(iter(metas.values()))[0]["topology_sha256"],
            "stage": {"dropped_before": total_dropped_before,
                "dropped_after": total_dropped, "invalid_identity": total_invalid,
                "unsupported_async": total_unsupported,
                "export_discarded": total_export_discarded, "pending": total_pending,
                "active_producers": total_active_producers, "io_error": any_io_error,
                "exported": total_exported},
            "exact": {field: exact_seal[field] for field in (
                "record_count", "dropped", "invalid_identity", "missing_registration",
                "unsupported_async", "unsupported_multi_handler", "pending", "io_error")},
        }
        return {**result, "stage_record_count": result["record_count"], **exact_result,
                "joined_records": joined, "stream_id": exact_stream_id,
                "raw_stage_events": stage_events,
                "raw_callback_events": callback_events,
                "measurement_kind": "exact_correlated"}
    if any(name.endswith((".ibce", ".exact.meta.json", ".exact.seal.json")) for name in files):
        raise ValueError("exact artifact is unsupported for this run type")
    if run_type == "stage_run" and measurement_kind != "stage_only":
        raise ValueError("dispatch measurement kind must be stage_only")
    return result


def _producer_source_set(files: dict[str, bytes]) -> tuple[str, list[dict[str, Any]]]:
    """Return the canonical inventory and digest of validated producer inputs."""
    from .exact_trace import parse_binary as parse_exact_binary

    inventory: list[dict[str, Any]] = []
    for name in sorted(files, key=lambda item: item.encode("utf-8")):
        data = files[name]
        if name.endswith(".ibct"):
            count = parse_binary(data)["record_count"]
        elif name.endswith(".ibce"):
            count = parse_exact_binary(data)["record_count"]
        elif name.endswith((".meta.json", ".seal.json")):
            _json_object(data)
            count = 1
        else:
            raise ValueError("unknown producer source-set file")
        inventory.append({"relative_filename": name,
                          "raw_sha256": hashlib.sha256(data).hexdigest(),
                          "record_count": count})
    canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False, allow_nan=False).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest(), inventory


def derive_identity_trace(files: dict[str, bytes], schemas: dict[str, dict[str, Any]],
                          schema_errors: Any, expected: dict[str, str], *,
                          producer_pid: int, producer_start_time_ticks: int,
                          topology_generation: str, topology_sha256: str) -> dict[str, Any]:
    """Fully validate producer evidence and derive collector-owned identity v2 bytes."""
    result = validate_directory_files(
        files, schemas, schema_errors, expected, run_type="exact_run",
        measurement_kind="exact_correlated", producer_pid=producer_pid,
        producer_start_time_ticks=producer_start_time_ticks,
        topology_generation=topology_generation, topology_sha256=topology_sha256,
        registration_contract_version="mw_perf.p0a_contract.v4")
    source_hash, inventory = _producer_source_set(files)
    common = {
        "schema": "mw_perf.p0a.identity_trace.v2", **expected,
        "producer": "mw_perf_collect", "finalized": True,
        "identity_verified": True, "conclusion": "pass",
        "stream_id": result["stream_id"], "producer_pid": producer_pid,
        "producer_start_time_ticks": producer_start_time_ticks,
        "topology_generation": topology_generation, "topology_sha256": topology_sha256,
        "source_set_sha256": source_hash,
    }
    records = [{**common, **joined} for joined in result["joined_records"]]
    data = b"".join(json.dumps(record, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
                    for record in records)
    return {"bytes": data, "records": records, "record_count": len(records),
            "content_sha256": hashlib.sha256(data).hexdigest(),
            "source_set_sha256": source_hash, "source_set": inventory,
            "trace_result": result}


def derive_hook_callgraph(identity_derivation: dict[str, Any],
                          resolution: dict[str, Any], resolution_bytes: bytes, *,
                          registration_contract_sha256: str) -> dict[str, Any]:
    """Derive the semantic boundary-chain claim from offline-regenerable inputs."""
    records = identity_derivation["records"]
    if (not records or identity_derivation.get("record_count") != len(records)
            or resolution.get("schema") not in (
                "mw_perf.p0a.resolution.v2", "mw_perf.p0a.remote_resolution.v1")):
        raise ValueError("hook callgraph requires identity and resolution proofs")
    identity = {field: records[0][field] for field in (
        "run_id", "capability_id", "path_id", "deployment_mode")}
    if any(record.get(field) != value for record in records
           for field, value in identity.items()):
        raise ValueError("identity trace records disagree")
    if any(resolution.get(field) != value for field, value in identity.items()):
        raise ValueError("resolution and identity trace disagree")

    declarations = resolution.get("declarations")
    matches = resolution.get("matches")
    if not isinstance(declarations, list) or not isinstance(matches, list):
        raise ValueError("resolution image bindings are unavailable")
    matches_by_identity = {(item.get("role"), item.get("name")): item
                           for item in matches if isinstance(item, dict)}
    resolved_images = []
    for declaration in declarations:
        if not isinstance(declaration, dict):
            raise ValueError("resolution declaration is malformed")
        match = matches_by_identity.get((declaration.get("role"), declaration.get("name")))
        observed = match.get("observed") if isinstance(match, dict) else None
        if (resolution.get("schema") == "mw_perf.p0a.remote_resolution.v1"
                and isinstance(match, dict)):
            observed = [match.get("ready_observation"),
                        match.get("completion_observation")]
        if (not isinstance(observed, list) or not observed
                or any(not isinstance(item, str) for item in observed)):
            raise ValueError("resolution declaration has no matched observation")
        observation_ids = [str(item) for item in observed]
        resolved_images.append({
            "role": declaration["role"], "name": declaration["name"],
            "source": declaration["source"],
            "expected_path": declaration.get("expected_path", declaration.get("path")),
            "sha256": declaration["sha256"],
            "declaration_sha256": hashlib.sha256(
                _canonical_bytes(declaration)).hexdigest(),
            "observation_ids": sorted(observation_ids),
        })
    resolved_images.sort(key=lambda item: tuple(item[field].encode("utf-8")
                         for field in ("role", "name", "source", "expected_path", "sha256")))
    if not {"sut", "extension"}.issubset({item["role"] for item in resolved_images}):
        raise ValueError("hook callgraph requires resolved sut and extension images")

    delivery_keys = sorted(({
        "writer_guid": record["writer_guid"],
        "sequence_high": record["sequence_high"],
        "sequence_low": record["sequence_low"],
        "reader_guid": record["reader_guid"],
        "callback_registration_id": record["callback_registration_id"],
    } for record in records), key=lambda item: (
        bytes.fromhex(item["writer_guid"]), item["sequence_high"], item["sequence_low"],
        bytes.fromhex(item["reader_guid"]), item["callback_registration_id"]))
    sample_count = len({(item["writer_guid"], item["sequence_high"], item["sequence_low"])
                        for item in delivery_keys})
    trace_result = identity_derivation["trace_result"]
    stage_count = trace_result.get("stage_record_count")
    if stage_count != sample_count + 2 * len(delivery_keys):
        raise ValueError("identity delivery set does not reconcile with stage count")
    catalog_hash = hashlib.sha256(_canonical_bytes(HOOK_BOUNDARY_CATALOG)).hexdigest()
    value = {
        "schema": "mw_perf.p0a.hook_callgraph.v2", **identity,
        "producer": "mw_perf_collect", "finalized": True, "conclusion": "pass",
        "claim": {
            "kind": "semantic_boundary_chain",
            "threat_model": "cooperative_sut",
            "coverage": "successful_exact_deliveries_only",
            "dynamic_stack_observation": "none",
            "runtime_function_addresses_observed": False,
            "source_to_build_provenance": "not_proven",
        },
        "inputs": {
            "registration_contract_sha256": registration_contract_sha256,
            "resolution": {
                "content_sha256": hashlib.sha256(resolution_bytes).hexdigest(),
                "declaration_sha256": resolution["declaration_sha256"],
            },
            "identity_trace": {
                "content_sha256": identity_derivation["content_sha256"],
                "source_set_sha256": identity_derivation["source_set_sha256"],
                "record_count": identity_derivation["record_count"],
            },
            "boundary_catalog_sha256": catalog_hash,
        },
        "boundary_catalog": HOOK_BOUNDARY_CATALOG,
        "runtime_binding": {
            "producer_pid": records[0]["producer_pid"],
            "producer_start_time_ticks": records[0]["producer_start_time_ticks"],
            "resolved_images": resolved_images,
        },
        "boundary_records": [
            {"logical_boundary": "publish_write", "record_count": sample_count},
            {"logical_boundary": "take_receive", "record_count": len(delivery_keys)},
            {"logical_boundary": "sync_dispatch", "record_count": len(delivery_keys)},
            {"logical_boundary": "final_callback_invocation", "record_count": len(delivery_keys)},
        ],
        "stage_record_count": stage_count,
        "callback_record_count": len(delivery_keys),
        "delivery_set": {
            "key_fields": ["writer_guid", "sequence_high", "sequence_low", "reader_guid",
                           "callback_registration_id"],
            "sha256": hashlib.sha256(_canonical_bytes(delivery_keys)).hexdigest(),
            "count": len(delivery_keys),
        },
        "temporal_ordering": {
            "clock_domain": "producer_monotonic",
            "constraints": [
                "publish_start_ns<=receive_start_ns",
                "receive_end_ns<=dispatch_start_ns",
                "dispatch_start_ns<=callback_start_ns",
                "callback_start_ns<=callback_end_ns",
                "callback_end_ns<=dispatch_end_ns",
            ],
        },
        "complete_set_equality": True,
        "zero_status": True,
        "zero_loss_and_exclusion_counters": True,
    }
    data = _canonical_bytes(value, newline=True)
    return {"value": value, "bytes": data,
            "content_sha256": hashlib.sha256(data).hexdigest(),
            "boundary_catalog_sha256": catalog_hash,
            "delivery_set_sha256": value["delivery_set"]["sha256"],
            "record_count": 1}


def derive_normalized_results(identity_derivation: dict[str, Any],
                              resolution: dict[str, Any], resolution_bytes: bytes,
                              hook_derivation: dict[str, Any], hook_bytes: bytes) -> dict[str, Any]:
    """Purely derive collector-owned communication events and delivery latencies."""
    records = identity_derivation.get("records")
    identity_bytes = identity_derivation.get("bytes")
    if (not isinstance(records, list) or not records or not isinstance(identity_bytes, bytes)
            or identity_derivation.get("record_count") != len(records)
            or identity_bytes != b"".join(_canonical_bytes(item, newline=True) for item in records)
            or identity_derivation.get("content_sha256") != hashlib.sha256(identity_bytes).hexdigest()
            or not isinstance(identity_derivation.get("source_set_sha256"), str)):
        raise ValueError("normalized results require an exact regenerated identity derivation")
    if (_json_object(resolution_bytes) != resolution
            or resolution.get("schema") not in (
                "mw_perf.p0a.resolution.v2", "mw_perf.p0a.remote_resolution.v1")):
        raise ValueError("normalized results require exact resolution bytes")
    hook_value = hook_derivation.get("value")
    if (not isinstance(hook_value, dict) or hook_value.get("schema") != "mw_perf.p0a.hook_callgraph.v2"
            or hook_derivation.get("bytes") != hook_bytes
            or hook_bytes != _canonical_bytes(hook_value, newline=True)
            or hook_derivation.get("content_sha256") != hashlib.sha256(hook_bytes).hexdigest()):
        raise ValueError("normalized results require exact regenerated hook bytes")
    hook_inputs = hook_value.get("inputs")
    if (not isinstance(hook_inputs, dict)
            or hook_inputs.get("resolution", {}).get("content_sha256") != hashlib.sha256(resolution_bytes).hexdigest()
            or hook_inputs.get("identity_trace") != {
                "content_sha256": identity_derivation["content_sha256"],
                "source_set_sha256": identity_derivation["source_set_sha256"],
                "record_count": len(records),
            }):
        raise ValueError("normalized results hook input binding mismatch")

    identity_fields = ("run_id", "capability_id", "path_id", "deployment_mode", "stream_id",
                       "producer_pid", "producer_start_time_ticks")
    common_identity = {field: records[0].get(field) for field in identity_fields}
    if any(record.get(field) != value for record in records
           for field, value in common_identity.items()):
        raise ValueError("normalized identity records disagree")
    if any(resolution.get(field) != common_identity[field]
           for field in ("run_id", "capability_id", "path_id", "deployment_mode")):
        raise ValueError("normalized resolution identity mismatch")
    if (not isinstance(common_identity["producer_pid"], int)
            or isinstance(common_identity["producer_pid"], bool)
            or common_identity["producer_pid"] <= 0
            or not isinstance(common_identity["producer_start_time_ticks"], int)
            or isinstance(common_identity["producer_start_time_ticks"], bool)
            or common_identity["producer_start_time_ticks"] < 0):
        raise ValueError("normalized producer identity is malformed")

    inputs = {
        "identity_trace": {
            "content_sha256": identity_derivation["content_sha256"],
            "source_set_sha256": identity_derivation["source_set_sha256"],
            "record_count": len(records),
        },
        "resolution_content_sha256": hashlib.sha256(resolution_bytes).hexdigest(),
        "hook_callgraph_content_sha256": hashlib.sha256(hook_bytes).hexdigest(),
    }
    common = {**common_identity, "producer": "mw_perf_collect", "finalized": True,
              "clock_domain": "producer_monotonic", "inputs": inputs}
    uint64_max = (1 << 64) - 1
    required_times = ("publish_start_ns", "publish_end_ns", "receive_start_ns",
                      "receive_end_ns", "dispatch_start_ns", "dispatch_end_ns",
                      "callback_start_ns", "callback_end_ns")
    delivery_by_key: dict[tuple[str, int, int, str, int], dict[str, Any]] = {}
    publishes: dict[tuple[str, int, int], tuple[int, int]] = {}
    for record in records:
        if (not isinstance(record.get("writer_guid"), str)
                or not isinstance(record.get("reader_guid"), str)
                or record["writer_guid"] == "0" * 32 or record["reader_guid"] == "0" * 32
                or any(re.fullmatch(r"[0-9a-f]{32}", record[field]) is None
                       for field in ("writer_guid", "reader_guid"))):
            raise ValueError("normalized GUID is malformed")
        if (not isinstance(record.get("sequence_high"), int)
                or isinstance(record.get("sequence_high"), bool)
                or not -(1 << 31) <= record["sequence_high"] < (1 << 31)
                or not isinstance(record.get("sequence_low"), int)
                or isinstance(record.get("sequence_low"), bool)
                or not 0 <= record["sequence_low"] < (1 << 32)):
            raise ValueError("normalized sequence is malformed")
        for field in required_times:
            value = record.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= uint64_max:
                raise ValueError("normalized timestamp is not uint64")
        for start, end in (("publish_start_ns", "publish_end_ns"),
                           ("receive_start_ns", "receive_end_ns"),
                           ("dispatch_start_ns", "dispatch_end_ns"),
                           ("callback_start_ns", "callback_end_ns")):
            if record[end] < record[start]:
                raise ValueError("normalized event has reversed timestamps")
        registration = record.get("callback_registration_id")
        if not isinstance(registration, int) or isinstance(registration, bool) or not 1 <= registration <= uint64_max:
            raise ValueError("normalized callback registration is malformed")
        sample = (record["writer_guid"], record["sequence_high"], record["sequence_low"])
        delivery = (*sample, record["reader_guid"], registration)
        publish_span = (record["publish_start_ns"], record["publish_end_ns"])
        if sample in publishes and publishes[sample] != publish_span:
            raise ValueError("fanout publish spans disagree")
        if delivery in delivery_by_key:
            raise ValueError("duplicate normalized DeliveryKey")
        publishes[sample] = publish_span
        delivery_by_key[delivery] = record

    def sample_order(key: tuple[str, int, int]) -> tuple[bytes, int, int]:
        return bytes.fromhex(key[0]), key[1], key[2]

    comm_records: list[dict[str, Any]] = []
    latency_records: list[dict[str, Any]] = []
    for sample in sorted(publishes, key=sample_order):
        start, end = publishes[sample]
        comm_records.append({"schema": "mw_perf.comm_event.v2", **common,
            "event_kind": "publish", "key_scope": "sample", "writer_guid": sample[0],
            "sequence_high": sample[1], "sequence_low": sample[2], "reader_guid": None,
            "callback_registration_id": None, "start_ns": start, "end_ns": end})
        deliveries = sorted((item for item in delivery_by_key if item[:3] == sample),
                            key=lambda item: (bytes.fromhex(item[3]), item[4]))
        ranked_events = []
        event_fields = (
            ("receive", "delivery", "receive_start_ns", "receive_end_ns"),
            ("dispatch", "delivery", "dispatch_start_ns", "dispatch_end_ns"),
            ("callback", "callback_delivery", "callback_start_ns", "callback_end_ns"),
        )
        for delivery in deliveries:
            for rank, fields in enumerate(event_fields):
                ranked_events.append((bytes.fromhex(delivery[3]), rank, delivery[4], delivery, fields))
        for _reader, _rank, _registration, delivery, fields in sorted(ranked_events):
            kind, scope, start_field, end_field = fields
            record = delivery_by_key[delivery]
            comm_records.append({"schema": "mw_perf.comm_event.v2", **common,
                "event_kind": kind, "key_scope": scope, "writer_guid": delivery[0],
                "sequence_high": delivery[1], "sequence_low": delivery[2],
                "reader_guid": delivery[3],
                "callback_registration_id": delivery[4] if kind == "callback" else None,
                "start_ns": record[start_field], "end_ns": record[end_field]})
        for delivery in deliveries:
            record = delivery_by_key[delivery]
            latency = record["callback_start_ns"] - record["publish_start_ns"]
            if latency < 0 or latency > uint64_max:
                raise ValueError("normalized latency is outside uint64")
            latency_records.append({"schema": "mw_perf.latency_sample.v2", **common,
                "measurement": "publish_start_to_callback_start", "unit": "ns",
                "writer_guid": delivery[0], "sequence_high": delivery[1],
                "sequence_low": delivery[2], "reader_guid": delivery[3],
                "callback_registration_id": delivery[4],
                "publish_start_ns": record["publish_start_ns"],
                "callback_start_ns": record["callback_start_ns"], "latency_ns": latency})
    outputs = {}
    for name, output_records in (("comm_events", comm_records),
                                 ("latency_samples", latency_records)):
        data = b"".join(_canonical_bytes(record, newline=True) for record in output_records)
        outputs[name] = {"bytes": data, "records": output_records,
                         "content_sha256": hashlib.sha256(data).hexdigest(),
                         "source_identity_sha256": identity_derivation["content_sha256"],
                         "source_identity_record_count": len(records),
                         "record_count": len(output_records)}
    return outputs


def derive_communication_latency(identity_derivation: dict[str, Any],
                                 resolution: dict[str, Any], resolution_bytes: bytes,
                                 hook_derivation: dict[str, Any], hook_bytes: bytes,
                                 normalized: dict[str, Any], *,
                                 registration_contract_sha256: str) -> dict[str, Any]:
    """Purely derive the run-scoped v2 communication-latency capability."""
    digest_pattern = r"[0-9a-f]{64}"
    if re.fullmatch(digest_pattern, registration_contract_sha256 or "") is None:
        raise ValueError("communication latency contract binding is malformed")
    records = identity_derivation.get("records")
    identity_bytes = identity_derivation.get("bytes")
    if (not isinstance(records, list) or not records or not isinstance(identity_bytes, bytes)
            or identity_derivation.get("record_count") != len(records)
            or identity_bytes != b"".join(_canonical_bytes(item, newline=True) for item in records)
            or identity_derivation.get("content_sha256") != hashlib.sha256(identity_bytes).hexdigest()
            or re.fullmatch(digest_pattern,
                            str(identity_derivation.get("source_set_sha256", ""))) is None):
        raise ValueError("communication latency requires regenerated identity bytes")
    if (_json_object(resolution_bytes) != resolution
            or resolution.get("schema") not in (
                "mw_perf.p0a.resolution.v2", "mw_perf.p0a.remote_resolution.v1")):
        raise ValueError("communication latency requires exact resolution bytes")
    hook_value = hook_derivation.get("value")
    if (not isinstance(hook_value, dict)
            or hook_derivation.get("bytes") != hook_bytes
            or hook_bytes != _canonical_bytes(hook_value, newline=True)
            or hook_derivation.get("content_sha256") != hashlib.sha256(hook_bytes).hexdigest()):
        raise ValueError("communication latency requires regenerated hook bytes")
    expected_normalized = derive_normalized_results(
        identity_derivation, resolution, resolution_bytes, hook_derivation, hook_bytes)
    if set(normalized) != {"comm_events", "latency_samples"}:
        raise ValueError("communication latency normalized result set mismatch")
    for name in ("comm_events", "latency_samples"):
        supplied, expected = normalized.get(name), expected_normalized[name]
        if (not isinstance(supplied, dict) or supplied.get("bytes") != expected["bytes"]
                or supplied.get("records") != expected["records"]
                or supplied.get("content_sha256") != expected["content_sha256"]
                or supplied.get("record_count") != expected["record_count"]):
            raise ValueError("communication latency normalized bytes mismatch")
    latency = normalized["latency_samples"]
    latency_bytes, latency_records = latency["bytes"], latency["records"]
    if (not latency_records
            or latency_bytes != b"".join(_canonical_bytes(item, newline=True)
                                        for item in latency_records)
            or hashlib.sha256(latency_bytes).hexdigest() != latency["content_sha256"]):
        raise ValueError("communication latency sample bytes are not canonical")

    identity_fields = ("run_id", "capability_id", "path_id", "deployment_mode", "stream_id",
                       "producer_pid", "producer_start_time_ticks")
    common_identity = {field: records[0].get(field) for field in identity_fields}
    if any(record.get(field) != value for record in records
           for field, value in common_identity.items()):
        raise ValueError("communication latency identity records disagree")
    if any(resolution.get(field) != common_identity[field]
           for field in ("run_id", "capability_id", "path_id", "deployment_mode")):
        raise ValueError("communication latency resolution identity mismatch")
    uint64_max = (1 << 64) - 1
    keys: list[dict[str, Any]] = []
    for identity_record, sample in zip(records, latency_records):
        key = {field: sample.get(field) for field in (
            "writer_guid", "sequence_high", "sequence_low", "reader_guid",
            "callback_registration_id")}
        if any(identity_record.get(field) != value for field, value in key.items()):
            raise ValueError("communication latency sample set differs from identity")
        if (sample.get("measurement") != "publish_start_to_callback_start"
                or sample.get("unit") != "ns"
                or sample.get("clock_domain") != "producer_monotonic"
                or sample.get("latency_ns") != sample.get("callback_start_ns")
                    - sample.get("publish_start_ns")):
            raise ValueError("communication latency boundary arithmetic mismatch")
        for field in ("publish_start_ns", "callback_start_ns", "latency_ns"):
            number = sample.get(field)
            if not isinstance(number, int) or isinstance(number, bool) or not 0 <= number <= uint64_max:
                raise ValueError("communication latency value is not uint64")
        keys.append(key)
    if len(keys) != len(records) or len({_canonical_bytes(key) for key in keys}) != len(keys):
        raise ValueError("communication latency DeliveryKey set is not unique")
    ordered_keys = sorted(keys, key=lambda item: (
        bytes.fromhex(item["writer_guid"]), item["sequence_high"], item["sequence_low"],
        bytes.fromhex(item["reader_guid"]), item["callback_registration_id"]))
    if keys != ordered_keys:
        raise ValueError("communication latency samples are not canonically ordered")
    delivery_hash = hashlib.sha256(_canonical_bytes(ordered_keys)).hexdigest()
    delivery_set, hook_inputs = hook_value.get("delivery_set"), hook_value.get("inputs")
    key_fields = ["writer_guid", "sequence_high", "sequence_low", "reader_guid",
                  "callback_registration_id"]
    if (not isinstance(delivery_set, dict) or delivery_set.get("key_fields") != key_fields
            or delivery_set.get("count") != len(ordered_keys)
            or delivery_set.get("sha256") != delivery_hash
            or hook_derivation.get("delivery_set_sha256") != delivery_hash
            or hook_value.get("zero_loss_and_exclusion_counters") is not True
            or not isinstance(hook_inputs, dict)
            or hook_inputs.get("registration_contract_sha256") != registration_contract_sha256
            or hook_inputs.get("resolution", {}).get("content_sha256") != hashlib.sha256(
                resolution_bytes).hexdigest()
            or hook_inputs.get("identity_trace") != {
                "content_sha256": identity_derivation["content_sha256"],
                "source_set_sha256": identity_derivation["source_set_sha256"],
                "record_count": len(records)}):
        raise ValueError("communication latency hook binding or delivery set mismatch")
    value = {
        "schema": "mw_perf.communication_latency.v2", **common_identity,
        "producer": "mw_perf_collect", "finalized": True,
        "inputs": {
            "registration_contract_sha256": registration_contract_sha256,
            "raw_source_set_sha256": identity_derivation["source_set_sha256"],
            "resolution_content_sha256": hashlib.sha256(resolution_bytes).hexdigest(),
            "identity_trace": {"content_sha256": identity_derivation["content_sha256"],
                               "record_count": len(records)},
            "hook_callgraph": {"content_sha256": hashlib.sha256(hook_bytes).hexdigest(),
                               "delivery_set_sha256": delivery_hash},
            "latency_samples": {"relative_path": "normalized/latency_samples.jsonl",
                "schema_version": "mw_perf.latency_sample.v2",
                "content_sha256": hashlib.sha256(latency_bytes).hexdigest(),
                "record_count": len(latency_records)}},
        "metric": {"measurement": "publish_start_to_callback_start",
            "start_boundary": "publish_write.start",
            "end_boundary": "final_callback_invocation.start",
            "formula": "callback_start_ns-publish_start_ns", "unit": "ns",
            "clock_domain": "producer_monotonic"},
        "sample_set": {"coverage": "successful_exact_deliveries_only",
            "key_fields": key_fields, "count": len(ordered_keys), "sha256": delivery_hash},
        "quality": {"nonempty": True, "unique_delivery_keys": True,
            "input_bindings_verified": True, "boundary_arithmetic_verified": True,
            "single_clock_domain": True, "sample_set_equals_identity_delivery_set": True,
            "zero_trace_loss_and_exclusion_counters": True},
        "support": {"level": "candidate_run_only", "aggregation": "none",
            "delivery_claim": "none", "performance_claim": "none",
            "metric_availability": "unsupported", "valid_for_comparison": False},
        "conclusion_scope": "run_scoped_metric_artifact_integrity", "conclusion": "pass"}
    data = _canonical_bytes(value, newline=True)
    return {"value": value, "bytes": data,
            "content_sha256": hashlib.sha256(data).hexdigest(),
            "latency_samples_content_sha256": latency["content_sha256"],
            "latency_samples_record_count": len(latency_records),
            "sample_set_sha256": delivery_hash, "sample_set_count": len(ordered_keys),
            "record_count": 1}


def derive_communication_delivery(identity_derivation: dict[str, Any],
                                  resolution: dict[str, Any], resolution_bytes: bytes,
                                  hook_derivation: dict[str, Any], hook_bytes: bytes,
                                  normalized: dict[str, Any],
                                  communication_latency: dict[str, Any], *,
                                  registration_contract_sha256: str,
                                  expectations: dict[str, Any]) -> dict[str, Any]:
    """Derive the narrow v8 transport-identity delivery correctness proof."""
    frozen_expectations = {
        "publish_count": 1000,
        "readers_per_sample": 1,
        "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records",
    }
    if expectations != frozen_expectations:
        raise ValueError("communication delivery expectations are not the frozen v8 values")
    expected_latency = derive_communication_latency(
        identity_derivation, resolution, resolution_bytes, hook_derivation, hook_bytes,
        normalized, registration_contract_sha256=registration_contract_sha256)
    if (not isinstance(communication_latency, dict)
            or communication_latency.get("value") != expected_latency["value"]
            or communication_latency.get("bytes") != expected_latency["bytes"]
            or communication_latency.get("content_sha256") != expected_latency["content_sha256"]
            or communication_latency.get("sample_set_sha256") != expected_latency["sample_set_sha256"]):
        raise ValueError("communication delivery latency capability mismatch")

    records = identity_derivation.get("records")
    trace_result = identity_derivation.get("trace_result")
    if not isinstance(records, list) or not isinstance(trace_result, dict):
        raise ValueError("communication delivery identity inputs are malformed")
    stage_events = trace_result.get("raw_stage_events")
    callback_events = trace_result.get("raw_callback_events")
    if not isinstance(stage_events, list) or not isinstance(callback_events, list):
        raise ValueError("communication delivery raw event order is unavailable")

    def checked_sample_key(item: dict[str, Any]) -> tuple[bytes, int, int]:
        writer, high, low = item.get("writer_guid"), item.get("sequence_high"), item.get("sequence_low")
        if (not isinstance(writer, str) or re.fullmatch(r"[0-9a-f]{32}", writer) is None
                or writer == "0" * 32 or not isinstance(high, int) or isinstance(high, bool)
                or not -(1 << 31) <= high < (1 << 31)
                or not isinstance(low, int) or isinstance(low, bool) or not 0 <= low < (1 << 32)
                or (high == -1 and low == 0)):
            raise ValueError("communication delivery SampleKey is malformed")
        return bytes.fromhex(writer), high, low

    def checked_delivery_key(item: dict[str, Any]) -> tuple[bytes, int, int, bytes]:
        reader = item.get("reader_guid")
        if (not isinstance(reader, str) or re.fullmatch(r"[0-9a-f]{32}", reader) is None
                or reader == "0" * 32):
            raise ValueError("communication delivery DeliveryKey is malformed")
        return (*checked_sample_key(item), bytes.fromhex(reader))

    def checked_callback_key(item: dict[str, Any]) -> tuple[bytes, int, int, bytes, int]:
        registration = item.get("callback_registration_id")
        if (not isinstance(registration, int) or isinstance(registration, bool)
                or not 1 <= registration < (1 << 64)):
            raise ValueError("communication delivery callback key is malformed")
        return (*checked_delivery_key(item), registration)

    publishes = [item for item in stage_events if item.get("kind") == "publish"]
    receives = [item for item in stage_events if item.get("kind") == "receive"]
    dispatches = [item for item in stage_events if item.get("kind") == "dispatch"]
    if len(publishes) + len(receives) + len(dispatches) != len(stage_events):
        raise ValueError("communication delivery stage kind is malformed")
    publish_keys = [checked_sample_key(item) for item in publishes]
    receive_keys = [checked_delivery_key(item) for item in receives]
    dispatch_keys = [checked_delivery_key(item) for item in dispatches]
    callback_keys = [checked_callback_key(item) for item in callback_events]
    if (publish_keys != sorted(publish_keys) or receive_keys != sorted(receive_keys)
            or dispatch_keys != sorted(dispatch_keys) or callback_keys != sorted(callback_keys)):
        raise ValueError("communication delivery raw per-stage order is not monotonic")
    if any(item.get("status") != 0 for item in stage_events + callback_events):
        raise ValueError("communication delivery requires zero status")

    expected_count = frozen_expectations["publish_count"]
    if not all(len(items) == expected_count for items in (
            publish_keys, receive_keys, dispatch_keys, callback_keys, records)):
        raise ValueError("communication delivery event count mismatch")
    sample_set, receive_set = set(publish_keys), set(receive_keys)
    dispatch_set = set(dispatch_keys)
    callback_delivery_set = {key[:4] for key in callback_keys}
    if (len(sample_set) != expected_count or len(receive_set) != expected_count
            or len(set(callback_keys)) != expected_count
            or receive_set != dispatch_set or receive_set != callback_delivery_set
            or {key[:3] for key in receive_set} != sample_set):
        raise ValueError("communication delivery exact set equality failed")
    readers_by_sample: dict[tuple[bytes, int, int], set[bytes]] = {}
    for key in receive_set:
        readers_by_sample.setdefault(key[:3], set()).add(key[3])
    if (set(readers_by_sample) != sample_set
            or any(len(readers) != 1 for readers in readers_by_sample.values())):
        raise ValueError("communication delivery reader fanout mismatch")

    identity_keys = [checked_callback_key(item) for item in records]
    comm = normalized.get("comm_events", {})
    latency = normalized.get("latency_samples", {})
    comm_records = comm.get("records") if isinstance(comm, dict) else None
    latency_records = latency.get("records") if isinstance(latency, dict) else None
    if (identity_keys != callback_keys or not isinstance(comm_records, list)
            or not isinstance(latency_records, list)
            or [checked_callback_key(item) for item in latency_records] != callback_keys):
        raise ValueError("communication delivery normalized or identity set mismatch")
    comm_counts = {kind: sum(item.get("event_kind") == kind for item in comm_records)
                   for kind in ("publish", "receive", "dispatch", "callback")}
    if any(value != expected_count for value in comm_counts.values()):
        raise ValueError("communication delivery normalized event count mismatch")

    sample_values = [{"writer_guid": key[0].hex(), "sequence_high": key[1],
                      "sequence_low": key[2]} for key in sorted(sample_set)]
    delivery_values = [{"writer_guid": key[0].hex(), "sequence_high": key[1],
        "sequence_low": key[2], "reader_guid": key[3].hex(),
        "callback_registration_id": key[4]} for key in callback_keys]
    sample_hash = hashlib.sha256(_canonical_bytes(sample_values)).hexdigest()
    delivery_hash = hashlib.sha256(_canonical_bytes(delivery_values)).hexdigest()
    hook_value = hook_derivation["value"]
    if (delivery_hash != hook_derivation.get("delivery_set_sha256")
            or delivery_hash != hook_value.get("delivery_set", {}).get("sha256")
            or delivery_hash != communication_latency.get("sample_set_sha256")):
        raise ValueError("communication delivery upstream delivery-set hash mismatch")
    identity = {field: records[0][field] for field in (
        "run_id", "capability_id", "path_id", "deployment_mode", "stream_id",
        "producer_pid", "producer_start_time_ticks")}
    loss_fields = ("dropped", "invalid_identity", "unsupported_async",
                   "missing_registration", "unsupported_multi_handler")
    if any(trace_result.get(field) != 0 for field in loss_fields):
        raise ValueError("communication delivery trace loss or exclusion detected")
    value = {
        "schema": "mw_perf.communication_delivery.v2", **identity,
        "producer": "mw_perf_collect", "finalized": True,
        "inputs": {
            "registration_contract_sha256": registration_contract_sha256,
            "raw_source_set_sha256": identity_derivation["source_set_sha256"],
            "resolution_content_sha256": hashlib.sha256(resolution_bytes).hexdigest(),
            "identity_trace": {"content_sha256": identity_derivation["content_sha256"],
                "source_set_sha256": identity_derivation["source_set_sha256"],
                "record_count": len(records)},
            "hook_callgraph": {"content_sha256": hashlib.sha256(hook_bytes).hexdigest(),
                "delivery_set_sha256": delivery_hash},
            "comm_events": {"content_sha256": comm["content_sha256"],
                "record_count": comm["record_count"]},
            "latency_samples": {"content_sha256": latency["content_sha256"],
                "record_count": latency["record_count"]},
            "communication_latency": {"content_sha256": communication_latency["content_sha256"],
                "sample_set_sha256": communication_latency["sample_set_sha256"]}},
        "expectations": frozen_expectations,
        "observed": {**comm_counts, "unique_sample_keys": len(sample_set),
            "unique_delivery_keys": len(receive_set),
            "distinct_reader_guids": len({key[3] for key in receive_set}),
            "sample_set": {"count": len(sample_values), "sha256": sample_hash},
            "delivery_set": {"count": len(delivery_values), "sha256": delivery_hash}},
        "correctness": {"complete_stage_set_equality": True,
            "complete_callback_set_equality": True, "unique_identities": True,
            "exactly_one_reader_per_sample": True,
            "publish_raw_order_monotonic": True, "receive_raw_order_monotonic": True,
            "dispatch_raw_order_monotonic": True,
            "callback_completion_record_order_monotonic": True,
            "missing_count": 0, "duplicate_count": 0, "out_of_order_count": 0,
            "nonzero_status_count": 0, "trace_loss_and_exclusions_count": 0},
        "scope": "transport_identity_events_only", "payload_ordinal_claim": "none",
        "callback_order_basis": "exact_record_submission_at_callback_exit",
        "callback_entry_order_claim": "none",
        "coverage": "full", "conclusion_scope": "run_scoped_transport_delivery_correctness",
        "conclusion": "pass"}
    data = _canonical_bytes(value, newline=True)
    return {"value": value, "bytes": data,
            "content_sha256": hashlib.sha256(data).hexdigest(),
            "expected_publish_count": expected_count,
            "expected_readers_per_sample": 1,
            "observed_publish_count": comm_counts["publish"],
            "observed_receive_count": comm_counts["receive"],
            "observed_dispatch_count": comm_counts["dispatch"],
            "observed_callback_count": comm_counts["callback"],
            "sample_set_sha256": sample_hash, "delivery_set_sha256": delivery_hash,
            "record_count": 1}


def derive_trace_quality(identity_derivation: dict[str, Any],
                         communication_delivery: dict[str, Any], *,
                         registration_contract_version: str,
                         registration_contract_sha256: str) -> dict[str, Any]:
    """Derive the v8 candidate-run trace-quality statement from validated raw facts."""
    if registration_contract_version not in (
            "mw_perf.p0a_contract.v8", "mw_perf.p0a_ohos_remote_contract.v1") or re.fullmatch(
            r"[0-9a-f]{64}", registration_contract_sha256 or "") is None:
        raise ValueError("trace quality registration contract binding is invalid")
    records = identity_derivation.get("records")
    identity_bytes = identity_derivation.get("bytes")
    source_set = identity_derivation.get("source_set")
    source_hash = identity_derivation.get("source_set_sha256")
    if (not isinstance(records, list) or not records or not isinstance(identity_bytes, bytes)
            or identity_derivation.get("record_count") != len(records)
            or identity_bytes != b"".join(_canonical_bytes(item, newline=True) for item in records)
            or identity_derivation.get("content_sha256") != hashlib.sha256(identity_bytes).hexdigest()
            or not isinstance(source_set, list)
            or source_hash != hashlib.sha256(_canonical_bytes(source_set)).hexdigest()
            or any(not isinstance(item, dict) or set(item) != {
                "relative_filename", "raw_sha256", "record_count"}
                for item in source_set)):
        raise ValueError("trace quality raw source binding is stale")

    delivery_value = communication_delivery.get("value")
    delivery_bytes = communication_delivery.get("bytes")
    if (not isinstance(delivery_value, dict) or not isinstance(delivery_bytes, bytes)
            or delivery_bytes != _canonical_bytes(delivery_value, newline=True)
            or communication_delivery.get("content_sha256") != hashlib.sha256(
                delivery_bytes).hexdigest()
            or communication_delivery.get("record_count") != 1
            or delivery_value.get("schema") != "mw_perf.communication_delivery.v2"
            or delivery_value.get("inputs", {}).get(
                "registration_contract_sha256") != registration_contract_sha256
            or delivery_value.get("inputs", {}).get("raw_source_set_sha256") != source_hash):
        raise ValueError("trace quality delivery artifact binding is stale")
    assert isinstance(delivery_value, dict)

    trace_result = identity_derivation.get("trace_result")
    facts = trace_result.get("trace_quality_facts") if isinstance(trace_result, dict) else None
    if not isinstance(facts, dict) or set(facts) != {
            "trace_mode", "sampling_algorithm", "sampling_permyriad",
            "topology_generation", "topology_sha256", "stage", "exact"}:
        raise ValueError("trace quality validated raw facts are unavailable")
    assert isinstance(trace_result, dict)
    stage, exact = facts.get("stage"), facts.get("exact")
    stage_fields = ("dropped_before", "dropped_after", "invalid_identity",
                    "unsupported_async", "export_discarded", "pending",
                    "active_producers", "io_error", "exported")
    exact_fields = ("record_count", "dropped", "invalid_identity",
                    "missing_registration", "unsupported_async",
                    "unsupported_multi_handler", "pending", "io_error")
    if (not isinstance(stage, dict) or set(stage) != set(stage_fields)
            or not isinstance(exact, dict) or set(exact) != set(exact_fields)
            or facts["trace_mode"] != "full" or facts["sampling_algorithm"] != 0
            or facts["sampling_permyriad"] != 10000
            or not isinstance(facts["topology_generation"], str)
            or not facts["topology_generation"]
            or re.fullmatch(r"[0-9a-f]{64}", str(facts["topology_sha256"])) is None):
        raise ValueError("trace quality mode, sampling, or topology is invalid")
    assert isinstance(stage, dict) and isinstance(exact, dict)
    numeric_stage = stage_fields[:-2] + ("exported",)
    numeric_exact = exact_fields[:-1]
    if (any(not isinstance(stage[field], int) or isinstance(stage[field], bool)
            or stage[field] < 0 for field in numeric_stage)
            or any(not isinstance(exact[field], int) or isinstance(exact[field], bool)
                   or exact[field] < 0 for field in numeric_exact)
            or not isinstance(stage["io_error"], bool)
            or not isinstance(exact["io_error"], bool)):
        raise ValueError("trace quality counters are malformed")

    stage_events = trace_result.get("raw_stage_events")
    callback_events = trace_result.get("raw_callback_events")
    if not isinstance(stage_events, list) or not isinstance(callback_events, list):
        raise ValueError("trace quality raw events are unavailable")
    publishes = [item for item in stage_events if item.get("kind") == "publish"]
    receives = [item for item in stage_events if item.get("kind") == "receive"]
    dispatches = [item for item in stage_events if item.get("kind") == "dispatch"]
    sample_keys = {(item.get("writer_guid"), item.get("sequence_high"),
                    item.get("sequence_low")) for item in publishes}
    delivery_keys = {(item.get("writer_guid"), item.get("sequence_high"),
                      item.get("sequence_low"), item.get("reader_guid")) for item in receives}
    dispatch_keys = {(item.get("writer_guid"), item.get("sequence_high"),
                      item.get("sequence_low"), item.get("reader_guid")) for item in dispatches}
    callback_delivery_keys = {(item.get("writer_guid"), item.get("sequence_high"),
                               item.get("sequence_low"), item.get("reader_guid"))
                              for item in callback_events}
    zero_stage = ("dropped_before", "dropped_after", "invalid_identity",
                  "unsupported_async", "export_discarded", "pending", "active_producers")
    zero_exact = ("dropped", "invalid_identity", "missing_registration",
                  "unsupported_async", "unsupported_multi_handler", "pending")
    stage_exported = len(stage_events)
    exact_exported = len(callback_events)
    sample_count, delivery_count = len(sample_keys), len(delivery_keys)
    stage_nonzero_status = sum(item.get("status") != 0 for item in stage_events)
    exact_nonzero_status = sum(item.get("status") != 0 for item in callback_events)
    if (any(stage[field] != 0 for field in zero_stage) or stage["io_error"]
            or any(exact[field] != 0 for field in zero_exact) or exact["io_error"]
            or stage_nonzero_status != 0 or exact_nonzero_status != 0
            or stage["exported"] != stage_exported or exact["record_count"] != exact_exported
            or trace_result.get("stage_record_count") != stage_exported
            or trace_result.get("record_count") != exact_exported
            or len(publishes) != sample_count or len(receives) != delivery_count
            or len(dispatches) != delivery_count or len(callback_events) != delivery_count
            or dispatch_keys != delivery_keys or callback_delivery_keys != delivery_keys
            or {key[:3] for key in delivery_keys} != sample_keys
            or delivery_value.get("observed", {}).get("unique_sample_keys") != sample_count
            or delivery_value.get("observed", {}).get("unique_delivery_keys") != delivery_count):
        raise ValueError("trace quality loss, counter, or correlation fact failed")

    identity = {field: records[0][field] for field in (
        "run_id", "capability_id", "path_id", "deployment_mode", "stream_id",
        "producer_pid", "producer_start_time_ticks")}
    if any(record.get(field) != value for record in records
           for field, value in identity.items()):
        raise ValueError("trace quality run identity mismatch")
    sample_hash = delivery_value["observed"]["sample_set"]["sha256"]
    delivery_hash = delivery_value["observed"]["delivery_set"]["sha256"]
    value = {
        "schema": ("mw_perf.trace_quality_ohos_remote.v1"
                   if registration_contract_version == "mw_perf.p0a_ohos_remote_contract.v1"
                   else "mw_perf.trace_quality.v1"), **identity,
        "producer": "mw_perf_collect", "finalized": True,
        "registration_contract": {"version": registration_contract_version,
            "sha256": registration_contract_sha256},
        "raw_source_set": {"sha256": source_hash, "file_count": len(source_set),
            "record_count": sum(item["record_count"] for item in source_set)},
        "topology": {"generation": facts["topology_generation"],
            "sha256": facts["topology_sha256"]},
        "trace_configuration": {"mode": "full", "sampling_algorithm": 0,
            "sampling_permyriad": 10000},
        "upstream_delivery": {"content_sha256": communication_delivery["content_sha256"],
            "record_count": 1, "schema_version": "mw_perf.communication_delivery.v2"},
        "reconciliation": {"stage_accepted": stage_exported,
            "stage_eligible": stage_exported, "stage_exported": stage_exported,
            "exact_accepted": exact_exported, "exact_eligible": exact_exported,
            "exact_exported": exact_exported},
        "counts": {"stage_records": stage_exported, "exact_records": exact_exported,
            "publish": len(publishes), "receive": len(receives), "dispatch": len(dispatches),
            "callback": len(callback_events), "unique_sample_keys": sample_count,
            "unique_delivery_keys": delivery_count},
        "counters": {"stage": stage, "exact": exact,
            "stage_nonzero_status": stage_nonzero_status,
            "exact_nonzero_status": exact_nonzero_status},
        "key_coverage": {"sample_key_fields": ["writer_guid", "sequence_high", "sequence_low"],
            "delivery_key_fields": ["writer_guid", "sequence_high", "sequence_low",
                                    "reader_guid", "callback_registration_id"],
            "sample_set_sha256": sample_hash, "delivery_set_sha256": delivery_hash,
            "complete_stage_and_exact_set_equality": True},
        "loss_accounting": {"sampling_exclusion_count": 0, "trace_loss_count": 0,
            "sampling_exclusion_distinct_from_trace_loss": True,
            "zero_loss_exclusion_status_pending_and_io": True},
        "support": {"level": ("remote_candidate_run_only"
                                if registration_contract_version == "mw_perf.p0a_ohos_remote_contract.v1"
                                else "candidate_run_only"), "formal_p0a_completion": False,
            "failure_code": "P0A_ARTIFACT"},
        "conclusion_scope": ("run_scoped_remote_trace_quality_artifact_integrity"
                             if registration_contract_version == "mw_perf.p0a_ohos_remote_contract.v1"
                             else "run_scoped_trace_quality_artifact_integrity"),
        "conclusion": "pass",
    }
    data = _canonical_bytes(value, newline=True)
    return {"value": value, "bytes": data,
            "content_sha256": hashlib.sha256(data).hexdigest(),
            "raw_source_set_sha256": source_hash,
            "stage_record_count": stage_exported, "exact_record_count": exact_exported,
            "sample_set_sha256": sample_hash, "delivery_set_sha256": delivery_hash,
            "record_count": 1}


def _canonical_bytes(value: Any, *, newline: bool = False) -> bytes:
    data = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")
    return data + (b"\n" if newline else b"")


def scan_directory_fd(dirfd: int) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for name in sorted(os.listdir(dirfd), key=lambda item: item.encode("utf-8")):
        if not name or name in (".", "..") or "/" in name or name.endswith(".part"):
            raise ValueError("unsafe DDS trace filename")
        info = os.stat(name, dir_fd=dirfd, follow_symlinks=False)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError("DDS trace entry is not a private regular file")
        if not (name.endswith(".meta.json") or name.endswith(".ibct") or
                name.endswith(".ibce") or name.endswith(".exact.meta.json") or
                name.endswith(".exact.seal.json") or name.endswith(".seal.json")):
            raise ValueError("unknown DDS trace file")
        fd = os.open(name, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0), dir_fd=dirfd)
        try:
            before = os.fstat(fd)
            compared = ("st_dev", "st_ino", "st_mode", "st_nlink", "st_uid",
                        "st_size", "st_mtime_ns")
            if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
                    or any(getattr(before, field) != getattr(info, field) for field in compared)):
                raise OSError("DDS trace entry changed before open")
            chunks: list[bytes] = []
            while chunk := os.read(fd, 1024 * 1024): chunks.append(chunk)
            after = os.fstat(fd)
            if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
                    != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)):
                raise OSError("DDS trace changed while reading")
            files[name] = b"".join(chunks)
        finally:
            os.close(fd)
    return files


def confirm_directory_unchanged(dirfd: int, producer_files: dict[str, bytes]) -> None:
    """Rescan producer evidence and confirm its names and bytes before commit."""
    if scan_directory_fd(dirfd) != producer_files:
        raise OSError("DDS trace directory changed after validation")
    os.fsync(dirfd)


def write_directory_finalization(dirfd: int, identity: dict[str, str], files: list[str]) -> bytes:
    value = {"schema": "mw_perf.comm_trace.finalization.v1", **identity,
             "finalized": True, "files": sorted(files, key=lambda item: item.encode("utf-8"))}
    data = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False,
                      allow_nan=False).encode("utf-8") + b"\n"
    temporary = f".directory_finalization.{os.getpid()}.part"
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
                 | getattr(os, "O_NOFOLLOW", 0), 0o600, dir_fd=dirfd)
    try:
        view = memoryview(data)
        while view: view = view[os.write(fd, view):]
        os.fsync(fd)
    except BaseException:
        try: os.unlink(temporary, dir_fd=dirfd)
        except OSError: pass
        raise
    finally:
        os.close(fd)
    os.replace(temporary, "directory_finalization.json", src_dir_fd=dirfd, dst_dir_fd=dirfd)
    os.fsync(dirfd)
    return data
