import hashlib
import json
import struct
from pathlib import Path

import pytest

from mw_perf_collect import core
from mw_perf_collect.campaign import validate_companion_object
from mw_perf_collect.runtime_fault_evaluation import (
    CASE_IDS,
    build_runtime_fault_companion,
    evaluate_runtime_fault_matrix,
    validate_runtime_fault_companion,
)


ERRORS = {
    "stage_loss": "DDS trace required zero-loss finalization failed",
    "callback_loss": "exact seal is not finalized",
    "producer_identity_mismatch": "DDS trace identity mismatch: run_id",
    "stage_status": "exact correlation requires zero status for every event",
    "callback_status": "exact correlation requires zero status for every event",
}


def _canonical(value):
    return core._canonical_json_bytes(value, newline=True)


def _sync_runtime_resolution(subject: Path):
    registered = json.loads(
        (subject / "registration/resolution_declarations.json").read_text())["declarations"]
    path = subject / "capability/resolution.json"
    value = json.loads(path.read_text())
    value["declarations"] = registered
    value["declaration_sha256"] = hashlib.sha256(
        core._canonical_json_bytes(registered)).hexdigest()
    by_source = {item["source"]: item for item in registered}
    for observation in value["observations"]:
        declaration = by_source[observation["source"]]
        observation.update({
            "resolved_path": declaration["expected_path"],
            "device": declaration["expected_device"],
            "inode": declaration["expected_inode"],
            "size": declaration["expected_size"],
            "sha256": declaration["sha256"],
        })
        observation["observation_id"] = core._observation_id(observation)
    value["observations"].sort(key=lambda item: item["observation_id"])
    value["matches"] = core._resolution_matches(registered, value["observations"])
    path.write_bytes(_canonical(value))


def _stage_record(sequence, kind, *, status=0):
    record = bytearray(64)
    struct.pack_into("<QQ", record, 0, sequence * 10, sequence * 10 + 5)
    record[16:32] = bytes.fromhex("33" * 16)
    struct.pack_into("<iI", record, 32, 0, sequence)
    record[40:56] = bytes(16) if kind == 1 else bytes.fromhex("44" * 16)
    struct.pack_into("<I", record, 56, 7)
    record[60:64] = bytes((kind, status, 1, 0))
    return bytes(record)


def _stage_binary(meta_raw, records, dropped=0, *, dropped_before=0,
                  invalid_identity=0, unsupported_async=0):
    payload = b"".join(records)
    header = bytearray(256)
    header[:8] = b"IBMWDCT\0"
    struct.pack_into("<HHHHB", header, 8, 1, 256, 1, 64, 1)
    header[24:40] = bytes.fromhex("11" * 16)
    header[40:72] = hashlib.sha256(meta_raw).digest()
    struct.pack_into("<IIQQQQQQ", header, 72, 123, 0, 456, 0, len(records),
                     dropped_before, dropped, invalid_identity)
    struct.pack_into("<Q", header, 128, unsupported_async)
    header[136:168] = hashlib.sha256(payload).digest()
    header[168] = 2
    struct.pack_into("<H", header, 170, 10000)
    return bytes(header) + payload


def _exact_record(sequence, *, status=0):
    record = bytearray(72)
    struct.pack_into("<QQ", record, 0, sequence * 10 + 6, sequence * 10 + 7)
    record[16:32] = bytes.fromhex("33" * 16)
    struct.pack_into("<iI", record, 32, 0, sequence)
    record[40:56] = bytes.fromhex("44" * 16)
    struct.pack_into("<Q", record, 56, 1 if sequence == 1 else 2)
    record[64:67] = bytes((1, status, 1))
    return bytes(record)


def _exact_binary(meta_raw, records, dropped=0, *, invalid_identity=0,
                  missing_registration=0, unsupported_async=0,
                  unsupported_multi_handler=0):
    payload = b"".join(records)
    header = bytearray(160)
    header[:8] = b"IBMWDCE\0"
    struct.pack_into("<HHHB", header, 8, 1, 160, 72, 1)
    header[16:32] = bytes.fromhex("11" * 16)
    header[32:64] = hashlib.sha256(meta_raw).digest()
    struct.pack_into("<I", header, 64, 123)
    struct.pack_into("<Q", header, 72, 456)
    struct.pack_into("<Q", header, 80, len(records))
    struct.pack_into("<Q", header, 88, dropped)
    struct.pack_into("<QQQQ", header, 96, invalid_identity, missing_registration,
                     unsupported_async, unsupported_multi_handler)
    header[128:160] = hashlib.sha256(payload).digest()
    return bytes(header) + payload


def _subject(root: Path, case_id: str) -> Path:
    subject = root / case_id
    for relative in ("raw/comm_trace", "capability", "registration", "logs"):
        (subject / relative).mkdir(parents=True, exist_ok=True)
    run_id = f"run-{case_id}"
    producer_run_id = "runtime-fault-wrong-run-id" if case_id == "producer_identity_mismatch" else run_id
    identity = {
        "run_id": producer_run_id,
        "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "path_id": "managed-loopback",
        "deployment_mode": "managed",
    }
    producer = {"producer_pid": 123, "producer_start_time_ticks": 456}
    stage_meta = {"schema": "mw_perf.dds_comm_trace.meta.v1", **identity, **producer,
                  "stream_id": "11" * 16, "trace_mode": "full",
                  "sampling_algorithm": 0, "sampling_permyriad": 10000}
    exact_meta = {"schema": "mw_perf.dds_exact_callback.meta.v1", **identity, **producer,
                  "stream_id": "11" * 16}
    stage_meta_raw = _canonical(stage_meta)
    exact_meta_raw = _canonical(exact_meta)
    stage_records = [_stage_record(1, kind) for kind in (1, 2, 3)]
    exact_records = [_exact_record(1)]
    if case_id == "stage_status":
        stage_records.append(_stage_record(2, 1, status=1))
    if case_id == "callback_status":
        exact_records.append(_exact_record(2, status=1))
    stage_dropped = int(case_id == "stage_loss")
    exact_dropped = int(case_id == "callback_loss")
    stage_seal = {"schema": "mw_perf.dds_comm_trace.seal.v1", **identity, **producer,
                  "stream_id": "11" * 16, "segment_count": 1,
                  "record_count": len(stage_records), "exported": len(stage_records),
                  "dropped": stage_dropped, "invalid_identity": 0, "unsupported_async": 0,
                  "trace_mode": "full", "sampling_algorithm": 0,
                  "sampling_permyriad": 10000, "finalized": True, "sealed": True,
                  "io_error": False, "pending": 0, "active_producers": 0,
                  "export_discarded": 0}
    exact_seal = {"schema": "mw_perf.dds_exact_callback.seal.v1", **identity, **producer,
                  "stream_id": "11" * 16, "record_count": len(exact_records),
                  "dropped": exact_dropped, "invalid_identity": 0,
                  "missing_registration": 0, "unsupported_async": 0,
                  "unsupported_multi_handler": 0, "finalized": case_id != "callback_loss",
                  "sealed": True, "io_error": False, "pending": 0}
    raw = subject / "raw/comm_trace"
    (raw / "stream.meta.json").write_bytes(stage_meta_raw)
    (raw / "stream.00000000.ibct").write_bytes(
        _stage_binary(stage_meta_raw, stage_records, stage_dropped))
    (raw / "stream.seal.json").write_bytes(_canonical(stage_seal))
    (raw / "stream.exact.meta.json").write_bytes(exact_meta_raw)
    (raw / "stream.ibce").write_bytes(_exact_binary(exact_meta_raw, exact_records, exact_dropped))
    (raw / "stream.exact.seal.json").write_bytes(_canonical(exact_seal))
    (subject / "raw/process.jsonl").write_bytes(b"{}\n")
    (subject / "raw/network.jsonl").write_bytes(b"{}\n")

    _contract, contract_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    (subject / "registration/contract.json").write_bytes(contract_raw)
    evidence = {
        "schema": "mw_perf.p0a_evidence_manifest.v1", "validation_profile": "p0a",
        "run_id": run_id, "capability_id": identity["capability_id"],
        "path_id": identity["path_id"], "deployment_mode": "managed",
        "registration_contract_sha256": hashlib.sha256(contract_raw).hexdigest(),
        "registration_contract_version": "mw_perf.p0a_contract.v8",
        "registration_contract_ref": "registration/contract.json", "run_type": "exact_run",
        "observer": False, "measurement_kind": "exact_correlated",
        "entries": [], "required": [], "conditional": [],
    }
    (subject / "capability/p0a_evidence_manifest.json").write_bytes(_canonical(evidence))
    declarations = [
        {"role": "extension", "name": "ibmw_dds_extension", "source": "maps",
         "expected_path": "/test/libibmw_dds_transport_fault_test.so", "sha256": "e" * 64,
         "expected_device": 1, "expected_inode": 2, "expected_size": 3},
        {"role": "sut", "name": "managed_exact_fault_candidate", "source": "exe",
         "expected_path": "/test/ibmw_dds_managed_exact_fault_candidate", "sha256": "f" * 64,
         "expected_device": 4, "expected_inode": 5, "expected_size": 6},
    ]
    resolution = {"schema": "mw_perf.p0a.resolution_declarations.v1",
                  "registration_contract_version": "mw_perf.p0a_contract.v8",
                  "declarations": declarations,
                  "declaration_sha256": hashlib.sha256(
                      core._canonical_json_bytes(declarations)).hexdigest()}
    (subject / "registration/resolution_declarations.json").write_bytes(_canonical(resolution))
    runtime_declarations = [dict(item) for item in declarations]
    observations = []
    for index, item in enumerate(runtime_declarations):
        observation = {
            "run_id": run_id, "pid": 123, "start_time_ticks": 456,
            "source": item["source"], "resolved_path": item["expected_path"],
            "device": item["expected_device"], "inode": item["expected_inode"],
            "size": item["expected_size"], "sha256": item["sha256"],
            "identity_source": "proc_exe" if item["source"] == "exe" else "proc_map_file",
        }
        if item["source"] == "maps":
            observation["mapping"] = {
                "start_address": "1000", "end_address": "2000", "permissions": "r-xp",
                "device": "00:01", "inode": item["expected_inode"], "deleted": False,
            }
        observation["observation_id"] = core._observation_id(observation)
        observations.append(observation)
    observations.sort(key=lambda item: item["observation_id"])
    runtime_resolution = {
        "schema": "mw_perf.p0a.resolution.v2", "run_id": run_id,
        "capability_id": identity["capability_id"], "path_id": identity["path_id"],
        "deployment_mode": "managed", "finalized": True, "resolved": True,
        "conclusion": "pass", "producer": "mw_perf_collect",
        "declaration_sha256": hashlib.sha256(
            core._canonical_json_bytes(runtime_declarations)).hexdigest(),
        "declarations": runtime_declarations, "observations": observations,
        "matches": core._resolution_matches(runtime_declarations, observations),
    }
    (subject / "capability/resolution.json").write_bytes(_canonical(runtime_resolution))
    manifest = {
        "schema_version": 1, "tool_version": "test", "run_id": run_id,
        "adapter": "generic", "mode": "managed", "run_status": "invalid",
        "scenario_contract_hash": "a" * 64, "metric_availability": "unsupported",
        "measurement_kind": "metadata", "start_time": 1, "end_time": 2,
        "pid": 123, "pid_start_time_ticks": 456, "states": ["PACKAGE"],
        "sample_count": 1, "sample_artifacts": {
            "process": {"relative_path": "raw/process.jsonl", "sha256": hashlib.sha256(b"{}\n").hexdigest(), "record_count": 1},
            "network": {"relative_path": "raw/network.jsonl", "sha256": hashlib.sha256(b"{}\n").hexdigest(), "record_count": 1},
        },
        "metrics_valid": True, "adapter_readiness": True, "candidate_only": True,
        "sut_returncode": 0,
        "dds_comm_trace": {"required": True, "status": "invalid", "error": f"ValueError: {ERRORS[case_id]}"},
    }
    (subject / "manifest.json").write_bytes(_canonical(manifest))
    validation = {
        "validation_profile": "p0a", "valid_for_collection": True,
        "valid_for_metric": False, "valid_for_comparison": False, "rules": [],
        "failure_codes": ["CANDIDATE_ONLY", "P0A_EVIDENCE"],
        "p0a_evidence": {"valid": False,
                         "failure_codes": ["P0A_ARTIFACT", "P0A_DDS_COMM_TRACE"]},
    }
    (subject / "validation.json").write_bytes(_canonical(validation))
    return subject


@pytest.fixture
def subjects(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    return {case_id: _subject(source, case_id) for case_id in CASE_IDS}


@pytest.fixture
def production_anchor(tmp_path):
    anchor = _subject(tmp_path / "anchor-source", "stage_loss")
    manifest_path = anchor / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["run_status"] = "completed"
    manifest["dds_comm_trace"] = {"required": True, "status": "exact_correlated"}
    manifest_path.write_bytes(_canonical(manifest))
    validation_path = anchor / "validation.json"
    validation = json.loads(validation_path.read_text())
    validation["p0a_evidence"]["failure_codes"] = ["P0A_ARTIFACT"]
    validation_path.write_bytes(_canonical(validation))
    resolution_path = anchor / "registration/resolution_declarations.json"
    resolution = json.loads(resolution_path.read_text())
    resolution["declarations"][0]["sha256"] = "a" * 64
    resolution["declarations"][1]["sha256"] = "b" * 64
    resolution["declaration_sha256"] = hashlib.sha256(
        core._canonical_json_bytes(resolution["declarations"])).hexdigest()
    resolution_path.write_bytes(_canonical(resolution))
    _sync_runtime_resolution(anchor)
    return anchor


@pytest.fixture(autouse=True)
def semantic_validator(monkeypatch):
    def validate(path, *, profile):
        assert profile == "p0a"
        return json.loads((path / "validation.json").read_text())
    monkeypatch.setattr("mw_perf_collect.runtime_fault_evaluation.core.validate_dir", validate)


def _evaluate(tmp_path, subjects, production_anchor):
    experiment = tmp_path / "experiment"
    report = evaluate_runtime_fault_matrix(subjects, experiment, production_anchor)
    return experiment, report


def _set_bound_counter(subject: Path, field: str, value: int = 1):
    raw = subject / "raw/comm_trace"
    if field.startswith("stage_"):
        offsets = {"stage_dropped_before": 104, "stage_dropped_after": 112,
                   "stage_invalid_identity": 120, "stage_unsupported_async": 128}
        seal_fields = {"stage_dropped_after": "dropped",
                       "stage_invalid_identity": "invalid_identity",
                       "stage_unsupported_async": "unsupported_async"}
        binary_path = raw / "stream.00000000.ibct"
        data = bytearray(binary_path.read_bytes())
        struct.pack_into("<Q", data, offsets[field], value)
        if field == "stage_dropped_before" and struct.unpack_from("<Q", data, 112)[0] < value:
            struct.pack_into("<Q", data, 112, value)
            seal_fields[field] = "dropped"
        binary_path.write_bytes(data)
        seal_field = seal_fields.get(field)
        if seal_field:
            seal_path = raw / "stream.seal.json"
            seal = json.loads(seal_path.read_text())
            seal[seal_field] = value
            seal_path.write_bytes(_canonical(seal))
        return

    offsets = {"exact_dropped": 88, "exact_invalid_identity": 96,
               "exact_missing_registration": 104, "exact_unsupported_async": 112,
               "exact_unsupported_multi_handler": 120}
    binary_path = raw / "stream.ibce"
    data = bytearray(binary_path.read_bytes())
    struct.pack_into("<Q", data, offsets[field], value)
    binary_path.write_bytes(data)
    seal_path = raw / "stream.exact.seal.json"
    seal = json.loads(seal_path.read_text())
    seal[field.removeprefix("exact_")] = value
    seal_path.write_bytes(_canonical(seal))


def _set_nonzero_status(subject: Path, stream: str):
    raw = subject / "raw/comm_trace"
    binary_path = raw / ("stream.00000000.ibct" if stream == "stage" else "stream.ibce")
    data = bytearray(binary_path.read_bytes())
    header_size, status_offset, hash_offset = ((256, 61, 136) if stream == "stage"
                                                else (160, 65, 128))
    data[header_size + status_offset] = 1
    data[hash_offset:hash_offset + 32] = hashlib.sha256(data[header_size:]).digest()
    binary_path.write_bytes(data)


def _set_identity_mismatch(subject: Path):
    raw = subject / "raw/comm_trace"
    for name in ("stream.meta.json", "stream.seal.json", "stream.exact.meta.json",
                 "stream.exact.seal.json"):
        path = raw / name
        value = json.loads(path.read_text())
        value["run_id"] = "runtime-fault-unrelated-run-id"
        path.write_bytes(_canonical(value))
    for name, meta_name, offset in (("stream.00000000.ibct", "stream.meta.json", 40),
                                    ("stream.ibce", "stream.exact.meta.json", 32)):
        path = raw / name
        data = bytearray(path.read_bytes())
        data[offset:offset + 32] = hashlib.sha256((raw / meta_name).read_bytes()).digest()
        path.write_bytes(data)


def test_runtime_matrix_snapshots_real_binary_faults_and_builds_companion(
        tmp_path, subjects, production_anchor):
    before = {case: (path / "manifest.json").read_bytes() for case, path in subjects.items()}
    experiment, report = _evaluate(tmp_path, subjects, production_anchor)
    assert report["finalized"] is True and report["conclusion"] == "pass"
    assert [case["case_id"] for case in report["cases"]] == list(CASE_IDS)
    assert all(case["sut_exit_zero"] and not case["directory_finalization_present"]
               for case in report["cases"])
    assert len({case["fault_build_identity_sha256"] for case in report["cases"]}) == 1
    assert report["fault_build_identity_sha256"] == report["cases"][0]["fault_build_identity_sha256"]
    assert report["production_build_identity_sha256"] == \
        report["cases"][0]["production_build_identity_sha256"]
    assert report["production_build_identity_sha256"] != report["fault_build_identity_sha256"]
    assert report["anchor"]["run_id"] == "run-stage_loss"
    assert report["instrumentation_relationship"] == \
        "test_instrumented_fault_producer_not_binary_identical_to_anchor"
    assert all((path / "manifest.json").read_bytes() == before[case]
               for case, path in subjects.items())
    descriptor = build_runtime_fault_companion(
        report, experiment, campaign_id="campaign", companion_id="matrix",
        object_store=tmp_path / "objects")
    assert descriptor["build_identity_sha256"] == report["fault_build_identity_sha256"]
    assert descriptor["role"] == "fault_runtime_matrix"
    object_path = (tmp_path / "objects/sha256" / descriptor["object_sha256"][:2]
                   / f'{descriptor["object_sha256"]}.tar')
    assert validate_companion_object(object_path, descriptor)["valid"]
    specialized = validate_runtime_fault_companion(object_path, descriptor)
    assert specialized["valid"]
    assert specialized["anchor"] == report["anchor"]
    assert specialized["production_build_identity_sha256"] == \
        report["production_build_identity_sha256"]
    assert specialized["fault_build_identity_sha256"] == \
        report["fault_build_identity_sha256"]


@pytest.mark.parametrize("case_id", CASE_IDS)
@pytest.mark.parametrize("field", [
    "stage_dropped_before", "stage_invalid_identity", "stage_unsupported_async",
    "exact_invalid_identity", "exact_missing_registration", "exact_unsupported_async",
    "exact_unsupported_multi_handler",
])
def test_every_case_rejects_nonzero_baseline_and_exclusions(tmp_path, subjects,
                                                            production_anchor, case_id, field):
    _set_bound_counter(subjects[case_id], field)
    with pytest.raises(ValueError, match="producer observation"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


@pytest.mark.parametrize("case_id,contaminant", [
    (case_id, contaminant)
    for case_id in CASE_IDS
    for contaminant in CASE_IDS
    if contaminant != case_id
])
def test_each_case_rejects_an_unrelated_primary_observation(tmp_path, subjects,
                                                            production_anchor, case_id, contaminant):
    subject = subjects[case_id]
    if contaminant == "stage_loss":
        _set_bound_counter(subject, "stage_dropped_after")
    elif contaminant == "callback_loss":
        _set_bound_counter(subject, "exact_dropped")
    elif contaminant == "producer_identity_mismatch":
        _set_identity_mismatch(subject)
    elif contaminant == "stage_status":
        _set_nonzero_status(subject, "stage")
    else:
        _set_nonzero_status(subject, "exact")
    with pytest.raises(ValueError, match="producer observation"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


@pytest.mark.parametrize("case_id", CASE_IDS)
def test_each_case_preserves_exact_finalized_requirement(
        tmp_path, subjects, production_anchor, case_id):
    path = subjects[case_id] / "raw/comm_trace/stream.exact.seal.json"
    value = json.loads(path.read_text())
    value["finalized"] = not value["finalized"]
    path.write_bytes(_canonical(value))
    with pytest.raises(ValueError, match="producer observation"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_observation_schema_is_closed_and_requires_every_field(
        tmp_path, subjects, production_anchor):
    _experiment, report = _evaluate(tmp_path, subjects, production_anchor)
    schema = json.loads((Path(__file__).parents[1] / "schemas/fault-runtime-matrix-result.schema.v1.json").read_text())
    observation = report["cases"][0]["injected_observation"]
    assert set(observation) == set(schema["$defs"]["observation"]["required"])
    for field in tuple(observation):
        changed = json.loads(json.dumps(report))
        del changed["cases"][0]["injected_observation"][field]
        assert core._schema_errors(changed, schema)
    changed = json.loads(json.dumps(report))
    changed["cases"][0]["injected_observation"]["unexpected"] = 0
    assert core._schema_errors(changed, schema)


@pytest.mark.parametrize("change", ["missing", "extra", "wrong"])
def test_exact_case_set_is_required(tmp_path, subjects, production_anchor, change):
    changed = dict(subjects)
    if change == "missing":
        changed.pop("stage_loss")
    elif change == "extra":
        changed["extra"] = subjects["stage_loss"]
    else:
        changed["wrong_case"] = changed.pop("stage_loss")
    with pytest.raises(ValueError, match="exact five"):
        evaluate_runtime_fault_matrix(changed, tmp_path / "experiment", production_anchor)


@pytest.mark.parametrize("mutation,match", [
    ("finalization", "finalization state"), ("exit", "finalization state"),
    ("counter", "mismatch"), ("status", "producer observation"),
    ("identity", "mismatch"), ("contract_hash", "contract binding"),
])
def test_fault_state_tampering_is_rejected_and_cleaned(
        tmp_path, subjects, production_anchor, mutation, match):
    subject = subjects["stage_loss"]
    if mutation == "finalization":
        (subject / "raw/comm_trace/directory_finalization.json").write_text("{}")
    elif mutation == "exit":
        path = subject / "manifest.json"; value = json.loads(path.read_text())
        value["sut_returncode"] = 1; path.write_bytes(_canonical(value))
    elif mutation == "counter":
        path = subject / "raw/comm_trace/stream.seal.json"; value = json.loads(path.read_text())
        value["dropped"] = 0; path.write_bytes(_canonical(value))
    elif mutation == "status":
        raw = subject / "raw/comm_trace/stream.00000000.ibct"
        meta = (subject / "raw/comm_trace/stream.meta.json").read_bytes()
        raw.write_bytes(_stage_binary(meta, [_stage_record(1, 1, status=1),
                                             _stage_record(1, 2), _stage_record(1, 3)], 1))
    elif mutation == "identity":
        path = subject / "raw/comm_trace/stream.exact.meta.json"; value = json.loads(path.read_text())
        value["run_id"] = "other"; path.write_bytes(_canonical(value))
    else:
        path = subject / "capability/p0a_evidence_manifest.json"; value = json.loads(path.read_text())
        value["registration_contract_sha256"] = "0" * 64; path.write_bytes(_canonical(value))
    experiment = tmp_path / "experiment"
    with pytest.raises(ValueError, match=match):
        evaluate_runtime_fault_matrix(subjects, experiment, production_anchor)
    assert not experiment.exists()


@pytest.mark.parametrize("mutation", [
    "stage_counter", "exact_counter", "stage_meta_hash", "exact_meta_hash",
    "stage_stream", "exact_stream", "stage_pid", "exact_start_ticks",
])
def test_binary_meta_seal_mismatches_are_rejected(
        tmp_path, subjects, production_anchor, mutation):
    raw = subjects["stage_loss"] / "raw/comm_trace"
    if mutation == "stage_counter":
        path = raw / "stream.seal.json"
        value = json.loads(path.read_text()); value["record_count"] += 1
        path.write_bytes(_canonical(value))
    elif mutation == "exact_counter":
        path = raw / "stream.exact.seal.json"
        value = json.loads(path.read_text()); value["record_count"] += 1
        path.write_bytes(_canonical(value))
    elif mutation == "stage_meta_hash":
        path, offset = raw / "stream.00000000.ibct", 40
        data = bytearray(path.read_bytes()); data[offset] ^= 1; path.write_bytes(data)
    elif mutation == "exact_meta_hash":
        path, offset = raw / "stream.ibce", 32
        data = bytearray(path.read_bytes()); data[offset] ^= 1; path.write_bytes(data)
    elif mutation == "stage_stream":
        path, offset = raw / "stream.00000000.ibct", 24
        data = bytearray(path.read_bytes()); data[offset] ^= 1; path.write_bytes(data)
    elif mutation == "exact_stream":
        path, offset = raw / "stream.ibce", 16
        data = bytearray(path.read_bytes()); data[offset] ^= 1; path.write_bytes(data)
    elif mutation == "stage_pid":
        path = raw / "stream.00000000.ibct"
        data = bytearray(path.read_bytes()); struct.pack_into("<I", data, 72, 124)
        path.write_bytes(data)
    else:
        path = raw / "stream.ibce"
        data = bytearray(path.read_bytes()); struct.pack_into("<Q", data, 72, 457)
        path.write_bytes(data)
    with pytest.raises(ValueError, match="mismatch"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_missing_nested_artifact_failure_is_rejected(tmp_path, subjects, production_anchor):
    path = subjects["stage_loss"] / "validation.json"
    value = json.loads(path.read_text())
    value["p0a_evidence"]["failure_codes"].remove("P0A_ARTIFACT")
    path.write_bytes(_canonical(value))
    with pytest.raises(ValueError, match="fail closed"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_mixed_fault_build_identities_are_rejected(tmp_path, subjects, production_anchor):
    path = subjects["stage_loss"] / "registration/resolution_declarations.json"
    value = json.loads(path.read_text())
    value["declarations"][0]["sha256"] = "9" * 64
    value["declaration_sha256"] = hashlib.sha256(
        core._canonical_json_bytes(value["declarations"])).hexdigest()
    path.write_bytes(_canonical(value))
    _sync_runtime_resolution(subjects["stage_loss"])
    with pytest.raises(ValueError, match="share one fault build identity"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_production_and_fault_build_identities_must_differ(
        tmp_path, subjects, production_anchor):
    source = subjects["stage_loss"] / "registration/resolution_declarations.json"
    destination = production_anchor / "registration/resolution_declarations.json"
    destination.write_bytes(source.read_bytes())
    _sync_runtime_resolution(production_anchor)
    with pytest.raises(ValueError, match="production and fault build identities must differ"):
        evaluate_runtime_fault_matrix(
            subjects, tmp_path / "experiment", production_anchor)


def test_registration_and_observation_declarations_must_match(
        tmp_path, subjects, production_anchor):
    path = subjects["stage_loss"] / "registration/resolution_declarations.json"
    value = json.loads(path.read_text())
    value["declarations"][0]["sha256"] = "9" * 64
    value["declaration_sha256"] = hashlib.sha256(
        core._canonical_json_bytes(value["declarations"])).hexdigest()
    path.write_bytes(_canonical(value))
    with pytest.raises(ValueError, match="do not match registration"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_closed_report_has_no_paths_and_anchor_remains_artifact_only(
        tmp_path, subjects, production_anchor):
    _experiment, report = _evaluate(tmp_path, subjects, production_anchor)
    encoded = json.dumps(report, sort_keys=True)
    assert str(tmp_path) not in encoded
    assert "/anchor" not in encoded and "payload/" not in encoded
    validation = json.loads((production_anchor / "validation.json").read_text())
    assert validation["p0a_evidence"]["failure_codes"] == ["P0A_ARTIFACT"]
    assert set(report["anchor"]) == {
        "run_id", "root_manifest_sha256", "evidence_manifest_sha256", "contract",
        "archive_tree_sha256"}


def test_stale_archived_validation_is_rejected(
        tmp_path, subjects, production_anchor, monkeypatch):
    path = subjects["stage_loss"] / "validation.json"
    value = json.loads(path.read_text())
    value["failure_codes"].append("STALE")
    path.write_bytes(_canonical(value))
    def validate(subject, *, profile):
        fresh = json.loads((subject / "validation.json").read_text())
        if fresh.get("failure_codes", [])[-1:] == ["STALE"]:
            fresh["failure_codes"].pop()
        return fresh
    monkeypatch.setattr("mw_perf_collect.runtime_fault_evaluation.core.validate_dir", validate)
    with pytest.raises(ValueError, match="does not equal fresh"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_source_mutation_during_revalidation_is_rejected(
        tmp_path, subjects, production_anchor, monkeypatch):
    def mutate(path, *, profile):
        source = subjects["callback_loss"] / "logs/mutation.txt"
        source.write_text("changed")
        return json.loads((path / "validation.json").read_text())
    monkeypatch.setattr("mw_perf_collect.runtime_fault_evaluation.core.validate_dir", mutate)
    with pytest.raises(ValueError, match="changed during semantic"):
        evaluate_runtime_fault_matrix(subjects, tmp_path / "experiment", production_anchor)


def test_companion_rejects_rewritten_report_and_extra_member(
        tmp_path, subjects, production_anchor):
    experiment, report = _evaluate(tmp_path, subjects, production_anchor)
    report_path = experiment / "payload/runtime_fault_matrix.json"
    rewritten = json.loads(report_path.read_text())
    rewritten["matrix_id"] = "runtime-fault-matrix-" + "0" * 24
    report_path.write_bytes(_canonical(rewritten))
    with pytest.raises(ValueError, match="report|manifest|inventory"):
        build_runtime_fault_companion(
            rewritten, experiment, campaign_id="campaign", companion_id="matrix",
            object_store=tmp_path / "objects")
    report_path.write_bytes(_canonical(report))
    anchor_manifest_path = experiment / "payload/anchor/manifest.json"
    anchor_manifest_raw = anchor_manifest_path.read_bytes()
    anchor_manifest = json.loads(anchor_manifest_raw)
    anchor_manifest["run_id"] = "replacement-anchor"
    anchor_manifest_path.write_bytes(_canonical(anchor_manifest))
    with pytest.raises(ValueError, match="anchor|report|inventory"):
        build_runtime_fault_companion(
            report, experiment, campaign_id="campaign", companion_id="matrix",
            object_store=tmp_path / "objects")
    anchor_manifest_path.write_bytes(anchor_manifest_raw)
    (experiment / "payload/unexpected.txt").write_text("unexpected")
    with pytest.raises(ValueError, match="unexpected member"):
        build_runtime_fault_companion(
            report, experiment, campaign_id="campaign", companion_id="matrix",
            object_store=tmp_path / "objects")


def test_runtime_validator_requires_descriptor(tmp_path, subjects, production_anchor):
    experiment, _report = _evaluate(tmp_path, subjects, production_anchor)
    descriptor = build_runtime_fault_companion(
        _report, experiment, campaign_id="campaign", companion_id="matrix",
        object_store=tmp_path / "objects")
    object_path = (tmp_path / "objects/sha256" / descriptor["object_sha256"][:2]
                   / f'{descriptor["object_sha256"]}.tar')
    result = validate_runtime_fault_companion(object_path)
    assert not result["valid"]
    assert result["failure_codes"] == ["RUNTIME_EXPECTED_DESCRIPTOR"]
