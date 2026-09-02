"""Fail-closed evaluation and packaging of the managed DDS runtime fault matrix."""
from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import tarfile
import tempfile
from pathlib import Path
from typing import Any, Mapping

from . import core
from .campaign import (
    PayloadSnapshot,
    build_companion_object_from_snapshot,
    capture_payload_snapshot,
    compute_archive_tree,
    compute_snapshot_archive_tree,
    validate_companion_object,
)
from .dds_trace import parse_binary as parse_stage_binary
from .exact_trace import parse_binary as parse_exact_binary
from .fault_evaluation import (
    _DIRECTORY_FLAGS,
    _copy_tree,
    _identity,
    _no_seal,
    _scan,
    _source_state,
    _strict_object,
)


CASE_IDS = (
    "callback_loss",
    "callback_status",
    "producer_identity_mismatch",
    "stage_loss",
    "stage_status",
)
_REPORT_NAME = "runtime_fault_matrix.json"
_MATRIX_MANIFEST = "runtime_matrix_manifest.json"
_MATRIX_EVIDENCE = "runtime_matrix_evidence_manifest.json"
_SCHEMA = "mw_perf.p0a.fault_runtime_matrix.v1"
_CONTRACT_VERSION = "mw_perf.p0a_contract.v8"
_INSTRUMENTATION_RELATIONSHIP = \
    "test_instrumented_fault_producer_not_binary_identical_to_anchor"
_EVIDENCE_CLAIM = \
    "collector_fail_closed_on_deterministic_test_instrumented_producer_anomalies"
_EXPECTED_ERRORS = {
    "stage_loss": ("stage_zero_loss", "DDS trace required zero-loss finalization failed"),
    "callback_loss": ("callback_zero_loss", "exact seal is not finalized"),
    "producer_identity_mismatch": ("producer_identity", "DDS trace identity mismatch: run_id"),
    "stage_status": ("stage_nonzero_status", "exact correlation requires zero status for every event"),
    "callback_status": ("callback_nonzero_status", "exact correlation requires zero status for every event"),
}


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical(value: Any) -> bytes:
    return core._canonical_json_bytes(value, newline=True)


def _build_identity(resolution: dict[str, Any]) -> tuple[str, dict[str, str]]:
    declarations = resolution.get("declarations")
    if (resolution.get("schema") != "mw_perf.p0a.resolution_declarations.v1"
            or resolution.get("registration_contract_version") != _CONTRACT_VERSION
            or not isinstance(declarations, list)
            or resolution.get("declaration_sha256") != _digest(
                core._canonical_json_bytes(declarations))):
        raise ValueError("subject resolution declaration binding is invalid")
    selected = []
    hashes: dict[str, str] = {}
    expected = {("sut", "exe"), ("extension", "maps")}
    for item in declarations:
        if not isinstance(item, dict) or set(item) != {
                "role", "name", "source", "expected_path", "sha256",
                "expected_device", "expected_inode", "expected_size"}:
            raise ValueError("subject resolution declaration shape is invalid")
        key = (item.get("role"), item.get("source"))
        if key not in expected or key in {(entry["role"], entry["source"])
                                          for entry in selected}:
            raise ValueError("subject resolution must declare exactly sut/exe and extension/maps")
        digest = item.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64 \
                or any(char not in "0123456789abcdef" for char in digest):
            raise ValueError("subject resolution image hash is invalid")
        selected.append({field: item[field] for field in ("role", "name", "source", "sha256")})
        hashes[item["role"]] = digest
    if len(selected) != 2:
        raise ValueError("subject resolution must declare exactly sut/exe and extension/maps")
    selected.sort(key=lambda item: (item["role"], item["name"], item["source"], item["sha256"]))
    formula = {"schema": "mw_perf.p0a.fault_build_identity.v1",
               "runtime_images": selected}
    return _digest(core._canonical_json_bytes(formula)), hashes


def _validate_runtime_resolution(files: Mapping[str, bytes], run_id: str,
                                 declarations: list[dict[str, Any]]) -> None:
    """Bind registered images to the independently archived runtime proof."""
    resolution, raw = _strict_subject_json(files, "capability/resolution.json",
                                            "runtime resolution")
    if (resolution.get("schema") != "mw_perf.p0a.resolution.v2"
            or resolution.get("run_id") != run_id
            or resolution.get("producer") != "mw_perf_collect"
            or resolution.get("finalized") is not True
            or resolution.get("resolved") is not True
            or resolution.get("conclusion") != "pass"
            or resolution.get("declaration_sha256") != _digest(
                core._canonical_json_bytes(resolution.get("declarations")))):
        raise ValueError("runtime resolution declaration binding is invalid")
    observations = resolution.get("observations")
    matches = resolution.get("matches")
    if not isinstance(observations, list) or not observations or not isinstance(matches, list):
        raise ValueError("runtime resolution observations are incomplete")
    if resolution.get("declarations") != declarations:
        raise ValueError("runtime resolution declarations do not match registration")
    by_id: dict[str, dict[str, Any]] = {}
    for item in observations:
        if (not isinstance(item, dict) or item.get("run_id") != run_id
                or item.get("source") not in ("exe", "maps")
                or item.get("observation_id") != core._observation_id(item)
                or item.get("observation_id") in by_id
                or not isinstance(item.get("resolved_path"), str)
                or not item["resolved_path"].startswith("/")
                or any(not isinstance(item.get(field), int) or isinstance(item.get(field), bool)
                       or item[field] < 0 for field in ("device", "inode", "size"))
                or not isinstance(item.get("sha256"), str)
                or len(item["sha256"]) != 64
                or any(ch not in "0123456789abcdef" for ch in item["sha256"])):
            raise ValueError("runtime resolution observation binding is invalid")
        by_id[item["observation_id"]] = item
    if observations != sorted(observations, key=lambda item: item["observation_id"]):
        raise ValueError("runtime resolution observations are not canonical")
    expected_matches = core._resolution_matches(declarations, observations)
    if matches != expected_matches or any(not item.get("observed") for item in matches):
        raise ValueError("runtime resolution declaration/observation matches are invalid")
    if {identifier for item in matches for identifier in item["observed"]} != set(by_id):
        raise ValueError("runtime resolution observations are not fully bound")
    for declaration, match in zip(declarations, matches):
        observed = [by_id[item] for item in match["observed"]]
        if (match.get("expected_sha256") != declaration["sha256"]
                or match.get("expected_path") != declaration["expected_path"]
                or match.get("expected_device") != declaration["expected_device"]
                or match.get("expected_inode") != declaration["expected_inode"]
                or match.get("expected_size") != declaration["expected_size"]
                or any((item["resolved_path"], item["device"], item["inode"],
                        item["size"], item["sha256"]) !=
                       (declaration["expected_path"], declaration["expected_device"],
                        declaration["expected_inode"], declaration["expected_size"],
                        declaration["sha256"]) for item in observed)):
            raise ValueError("runtime resolution declaration/observation binding is invalid")


def _archived_payload(object_path: Path) -> tuple[PayloadSnapshot, dict[str, Any], bytes]:
    raw = Path(object_path).read_bytes()
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
        members = list(archive)
        payload: dict[str, bytes] = {}
        manifest_raw = None
        for member in members:
            if not member.isreg() or member.mode != 0o600 or member.uid != 0 or member.gid != 0 \
                    or member.uname or member.gname or member.mtime != 0:
                raise ValueError("runtime companion tar metadata is not canonical")
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError("runtime companion tar member is unreadable")
            data = stream.read()
            if member.name == "bundle_manifest.json":
                manifest_raw = data
            elif member.name.startswith("payload/"):
                relative = member.name.removeprefix("payload/")
                if relative in payload:
                    raise ValueError("runtime companion tar contains duplicate payload")
                payload[relative] = data
            else:
                raise ValueError("runtime companion tar has an unexpected member")
    if manifest_raw is None or members[0].name != "bundle_manifest.json":
        raise ValueError("runtime companion tar manifest is missing or unordered")
    manifest = _strict_object(manifest_raw, "bundle manifest")
    expected = ["bundle_manifest.json"] + ["payload/" + name for name in sorted(
        payload, key=lambda item: item.encode("utf-8"))]
    if [item.name for item in members] != expected:
        raise ValueError("runtime companion tar member order is not canonical")
    return PayloadSnapshot(payload), manifest, raw


def _analyze_anchor(files: Mapping[str, bytes], *, anchor_path: Path | None) -> dict[str, Any]:
    manifest, manifest_raw = _strict_subject_json(files, "manifest.json", "anchor root manifest")
    validation, _validation_raw = _strict_subject_json(files, "validation.json", "anchor validation")
    evidence, evidence_raw = _strict_subject_json(
        files, "capability/p0a_evidence_manifest.json", "anchor evidence manifest")
    resolution, _resolution_raw = _strict_subject_json(
        files, "registration/resolution_declarations.json", "anchor resolution declarations")
    contract, contract_raw = _strict_subject_json(
        files, "registration/contract.json", "anchor contract")
    if core._schema_errors(manifest, _schema("result.schema.v1.json")):
        raise ValueError("anchor root manifest schema is invalid")
    if core._schema_errors(validation, _schema("validation.schema.v1.json")):
        raise ValueError("anchor validation schema is invalid")
    if core._schema_errors(evidence, _schema("p0a-evidence-manifest.schema.v1.json")):
        raise ValueError("anchor evidence manifest schema is invalid")
    _trusted, trusted_raw = core._trusted_p0a_contract(_CONTRACT_VERSION)
    contract_hash = _digest(contract_raw)
    if (contract_raw != trusted_raw
            or contract.get("registration_contract_version") != _CONTRACT_VERSION
            or evidence.get("registration_contract_version") != _CONTRACT_VERSION
            or evidence.get("registration_contract_sha256") != contract_hash
            or evidence.get("run_id") != manifest.get("run_id")):
        raise ValueError("anchor trusted contract binding is invalid")
    trace = manifest.get("dds_comm_trace", {})
    nested = validation.get("p0a_evidence")
    nested_codes = nested.get("failure_codes", []) if isinstance(nested, dict) else []
    if (manifest.get("run_status") != "completed"
            or manifest.get("candidate_only") is not True
            or manifest.get("metric_availability") != "unsupported"
            or trace.get("status") != "exact_correlated"
            or "cleanup_error" in manifest
            or manifest.get("stop_escalated_sigkill") is True
            or validation.get("valid_for_collection") is not True
            or validation.get("valid_for_metric") is not False
            or validation.get("valid_for_comparison") is not False
            or not isinstance(nested, dict) or nested.get("valid") is not False
            or "P0A_ARTIFACT" not in nested_codes
            or "P0A_DDS_COMM_TRACE" in nested_codes):
        raise ValueError("anchor is not the required exact-correlated P0A candidate")
    if anchor_path is None:
        with tempfile.TemporaryDirectory(prefix="mw-perf-runtime-anchor-") as temporary:
            validation_path = Path(temporary)
            for relative, data in files.items():
                target = validation_path / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            revalidated = core.validate_dir(validation_path, profile="p0a")
    else:
        revalidated = core.validate_dir(anchor_path, profile="p0a")
    if not core._validation_matches_current(validation, revalidated):
        raise ValueError("archived anchor validation does not equal fresh semantic validation")
    fresh_nested = revalidated.get("p0a_evidence", {})
    fresh_codes = fresh_nested.get("failure_codes", [])
    if (revalidated.get("valid_for_collection") is not True
            or revalidated.get("valid_for_metric") is not False
            or revalidated.get("valid_for_comparison") is not False
            or fresh_nested.get("valid") is not False
            or "P0A_ARTIFACT" not in fresh_codes
            or "P0A_DDS_COMM_TRACE" in fresh_codes):
        raise ValueError("anchor semantic revalidation did not preserve candidate-only status")
    declarations = resolution.get("declarations")
    if not isinstance(declarations, list):
        raise ValueError("anchor resolution declarations are malformed")
    _validate_runtime_resolution(files, manifest["run_id"], declarations)
    build_identity, _runtime_images = _build_identity(resolution)
    return {
        "run_id": manifest["run_id"],
        "root_manifest_sha256": _digest(manifest_raw),
        "evidence_manifest_sha256": _digest(evidence_raw),
        "contract": {"id": _CONTRACT_VERSION, "sha256": contract_hash},
        "production_build_identity_sha256": build_identity,
    }


def _schema(name: str) -> dict[str, Any]:
    return _strict_object(Path(__file__).parent.joinpath("schemas", name).read_bytes(), name)


def _files_for_prefix(files: Mapping[str, bytes], prefix: str) -> dict[str, bytes]:
    marker = prefix + "/"
    result = {name[len(marker):]: data for name, data in files.items() if name.startswith(marker)}
    if not result:
        raise ValueError("runtime matrix subject snapshot is empty")
    return result


def _strict_subject_json(files: Mapping[str, bytes], relative: str, label: str) -> tuple[dict[str, Any], bytes]:
    try:
        raw = files[relative]
    except KeyError as exc:
        raise ValueError(f"subject {label} is missing") from exc
    return _strict_object(raw, label), raw


def _single(files: Mapping[str, bytes], suffix: str, label: str) -> tuple[str, bytes]:
    matches = sorted(name for name in files if name.startswith("raw/comm_trace/") and name.endswith(suffix))
    if len(matches) != 1:
        raise ValueError(f"subject requires exactly one {label}")
    return matches[0], files[matches[0]]


_PRODUCER_IDENTITY_FIELDS = ("run_id", "capability_id", "path_id", "deployment_mode")


def _producer_artifacts(files: Mapping[str, bytes], manifest: dict[str, Any]) -> dict[str, Any]:
    stage_meta_names = sorted(name for name in files if name.startswith("raw/comm_trace/")
                              and name.endswith(".meta.json")
                              and not name.endswith(".exact.meta.json"))
    stage_seal_names = sorted(name for name in files if name.startswith("raw/comm_trace/")
                              and name.endswith(".seal.json")
                              and not name.endswith(".exact.seal.json"))
    if len(stage_meta_names) != 1 or len(stage_seal_names) != 1:
        raise ValueError("subject stage producer metadata is incomplete")
    stage_meta_raw = files[stage_meta_names[0]]
    stage_meta = _strict_object(stage_meta_raw, "stage meta")
    stage_seal = _strict_object(files[stage_seal_names[0]], "stage seal")
    _exact_meta_name, exact_meta_raw = _single(files, ".exact.meta.json", "exact meta")
    _exact_seal_name, exact_seal_raw = _single(files, ".exact.seal.json", "exact seal")
    exact_meta = _strict_object(exact_meta_raw, "exact meta")
    exact_seal = _strict_object(exact_seal_raw, "exact seal")
    records = (stage_meta, stage_seal, exact_meta, exact_seal)
    identity = {field: stage_meta.get(field) for field in _PRODUCER_IDENTITY_FIELDS}
    if (any(not isinstance(value, str) or not value for value in identity.values())
            or any(record.get(field) != value for record in records
                   for field, value in identity.items())):
        raise ValueError("producer artifact identity mismatch")

    stream_id = stage_meta.get("stream_id")
    if (not isinstance(stream_id, str) or not stream_id
            or any(record.get("stream_id") != stream_id for record in records)):
        raise ValueError("producer stream identity mismatch")
    stage_names = sorted(name for name in files if name.startswith("raw/comm_trace/")
                         and name.endswith(".ibct"))
    if not stage_names:
        raise ValueError("subject requires at least one stage binary")
    stage_items = [(name, parse_stage_binary(files[name])) for name in stage_names]
    stage_items.sort(key=lambda item: item[1]["segment_index"])
    ordinal = 0
    prior_dropped = stage_items[0][1]["dropped_before"]
    prior_invalid = prior_unsupported = 0
    stage_events: list[dict[str, Any]] = []
    for index, (_name, parsed) in enumerate(stage_items):
        if (parsed["stream_id"] != stream_id
                or parsed["meta_sha256"] != _digest(stage_meta_raw)):
            raise ValueError("stage binary/meta hash or stream mismatch")
        if (parsed["producer_pid"] != stage_meta.get("producer_pid")
                or parsed["producer_start_time_ticks"] != stage_meta.get(
                    "producer_start_time_ticks")
                or parsed["mode"] != stage_meta.get("trace_mode")
                or parsed["sampling_algorithm"] != stage_meta.get("sampling_algorithm")
                or parsed["sampling_permyriad"] != stage_meta.get("sampling_permyriad")):
            raise ValueError("stage binary/meta producer identity mismatch")
        if (parsed["segment_index"] != index or parsed["first_ordinal"] != ordinal
                or parsed["dropped_before"] != prior_dropped
                or parsed["dropped_after"] < parsed["dropped_before"]
                or parsed["invalid_identity"] < prior_invalid
                or parsed["unsupported_async"] < prior_unsupported):
            raise ValueError("stage binary segment counters or ordinals mismatch")
        ordinal += parsed["record_count"]
        prior_dropped = parsed["dropped_after"]
        prior_invalid = parsed["invalid_identity"]
        prior_unsupported = parsed["unsupported_async"]
        stage_events.extend(parsed["events"])
    last_stage = stage_items[-1][1]
    stage_checks = {
        "producer_pid": last_stage["producer_pid"],
        "producer_start_time_ticks": last_stage["producer_start_time_ticks"],
        "trace_mode": last_stage["mode"],
        "sampling_algorithm": last_stage["sampling_algorithm"],
        "sampling_permyriad": last_stage["sampling_permyriad"],
        "segment_count": len(stage_items), "record_count": ordinal, "exported": ordinal,
        "dropped": last_stage["dropped_after"],
        "invalid_identity": last_stage["invalid_identity"],
        "unsupported_async": last_stage["unsupported_async"],
    }
    if any(stage_seal.get(key) != value for key, value in stage_checks.items()):
        raise ValueError("stage seal/binary counters or header mismatch")
    if (stage_seal.get("finalized") is not True or stage_seal.get("sealed") is not True
            or stage_seal.get("io_error") is not False or stage_seal.get("pending") != 0
            or stage_seal.get("active_producers") != 0
            or stage_seal.get("export_discarded") != 0):
        raise ValueError("stage seal finalization mismatch")

    _exact_name, exact_raw = _single(files, ".ibce", "exact binary")
    exact = parse_exact_binary(exact_raw)
    if (exact["stream_id"] != stream_id or exact["meta_sha256"] != _digest(exact_meta_raw)):
        raise ValueError("exact binary/meta hash or stream mismatch")
    exact_fields = ("producer_pid", "producer_start_time_ticks", "record_count", "dropped",
                    "invalid_identity", "missing_registration", "unsupported_async",
                    "unsupported_multi_handler")
    if (any(exact.get(field) != exact_meta.get(field) for field in exact_fields[:2])
            or any(exact_seal.get(field) != exact.get(field) for field in exact_fields)):
        raise ValueError("exact seal/binary counters or header mismatch")
    if (exact_seal.get("sealed") is not True or exact_seal.get("io_error") is not False
            or exact_seal.get("pending") != 0
            or not isinstance(exact_seal.get("finalized"), bool)):
        raise ValueError("exact seal finalization mismatch")
    if any(record.get("producer_pid") != manifest.get("pid")
           or record.get("producer_start_time_ticks") != manifest.get("pid_start_time_ticks")
           for record in records):
        raise ValueError("producer PID/start ticks do not match subject")
    return {"identity": identity, "stage_first": stage_items[0][1],
            "stage_final": last_stage, "stage_seal": stage_seal, "exact": exact,
            "exact_seal": exact_seal, "stage_events": stage_events,
            "exact_events": exact["events"]}


def _producer_observation(case_id: str, files: Mapping[str, bytes], manifest: dict[str, Any],
                          evidence: dict[str, Any]) -> dict[str, Any]:
    artifacts = _producer_artifacts(files, manifest)
    stage_first = artifacts["stage_first"]
    stage_final = artifacts["stage_final"]
    exact = artifacts["exact"]
    exact_seal = artifacts["exact_seal"]
    stage_events, exact_events = artifacts["stage_events"], artifacts["exact_events"]
    if not stage_events or not exact_events:
        raise ValueError("subject producer records are incomplete")

    run_id = manifest.get("run_id")
    identity = artifacts["identity"]
    if (any(identity.get(field) != evidence.get(field) for field in _PRODUCER_IDENTITY_FIELDS[1:])
            or evidence.get("run_id") != run_id):
        raise ValueError("producer identity fields do not match subject evidence")
    producer_ids = {identity["run_id"]}
    observation = {
        "stage_dropped_before": stage_first["dropped_before"],
        "stage_dropped_after": stage_final["dropped_after"],
        "stage_invalid_identity": stage_final["invalid_identity"],
        "stage_unsupported_async": stage_final["unsupported_async"],
        "exact_dropped": exact["dropped"],
        "exact_invalid_identity": exact["invalid_identity"],
        "exact_missing_registration": exact["missing_registration"],
        "exact_unsupported_async": exact["unsupported_async"],
        "exact_unsupported_multi_handler": exact["unsupported_multi_handler"],
        "stage_nonzero_status_count": sum(event["status"] != 0 for event in stage_events),
        "exact_nonzero_status_count": sum(event["status"] != 0 for event in exact_events),
        "producer_run_id_matches_subject": producer_ids == {run_id},
    }
    counter_fields = tuple(key for key in observation if key != "producer_run_id_matches_subject")
    if any(not isinstance(observation[key], int) or isinstance(observation[key], bool)
           or observation[key] < 0 for key in counter_fields):
        raise ValueError("producer loss counters are malformed")
    exact_finalized = exact_seal.get("finalized")
    exclusions_zero = all(observation[key] == 0 for key in (
        "stage_invalid_identity", "stage_unsupported_async", "exact_invalid_identity",
        "exact_missing_registration", "exact_unsupported_async",
        "exact_unsupported_multi_handler"))
    common = observation["stage_dropped_before"] == 0 and exclusions_zero
    expected = {
        "stage_loss": (common and observation["stage_dropped_after"] > 0
                        and observation["exact_dropped"] == 0
                       and observation["stage_nonzero_status_count"] == 0
                       and observation["exact_nonzero_status_count"] == 0
                       and observation["producer_run_id_matches_subject"]
                       and exact_finalized is True),
        "callback_loss": (common and observation["stage_dropped_after"] == 0
                           and observation["exact_dropped"] > 0
                          and observation["stage_nonzero_status_count"] == 0
                          and observation["exact_nonzero_status_count"] == 0
                          and observation["producer_run_id_matches_subject"]
                          and exact_finalized is False),
        "producer_identity_mismatch": (common and observation["stage_dropped_after"] == 0
                                       and observation["exact_dropped"] == 0
                                       and observation["stage_nonzero_status_count"] == 0
                                       and observation["exact_nonzero_status_count"] == 0
                                       and len(producer_ids) == 1
                                       and not observation["producer_run_id_matches_subject"]
                                       and exact_finalized is True),
        "stage_status": (common and observation["stage_dropped_after"] == 0
                          and observation["exact_dropped"] == 0
                         and observation["stage_nonzero_status_count"] > 0
                         and observation["exact_nonzero_status_count"] == 0
                         and observation["producer_run_id_matches_subject"]
                         and exact_finalized is True),
        "callback_status": (common and observation["stage_dropped_after"] == 0
                             and observation["exact_dropped"] == 0
                            and observation["stage_nonzero_status_count"] == 0
                            and observation["exact_nonzero_status_count"] > 0
                            and observation["producer_run_id_matches_subject"]
                            and exact_finalized is True),
    }
    if not expected[case_id]:
        raise ValueError("subject producer observation does not match its runtime fault case")
    return observation


def _analyze_subject(case_id: str, files: Mapping[str, bytes], *, subject_path: Path | None) -> dict[str, Any]:
    manifest, manifest_raw = _strict_subject_json(files, "manifest.json", "root manifest")
    validation, _validation_raw = _strict_subject_json(files, "validation.json", "validation")
    evidence, evidence_raw = _strict_subject_json(
        files, "capability/p0a_evidence_manifest.json", "evidence manifest")
    resolution, _resolution_raw = _strict_subject_json(
        files, "registration/resolution_declarations.json", "resolution declarations")
    contract, contract_raw = _strict_subject_json(files, "registration/contract.json", "contract")
    if core._schema_errors(manifest, _schema("result.schema.v1.json")):
        raise ValueError("subject root manifest schema is invalid")
    if core._schema_errors(validation, _schema("validation.schema.v1.json")):
        raise ValueError("subject validation schema is invalid")
    if core._schema_errors(evidence, _schema("p0a-evidence-manifest.schema.v1.json")):
        raise ValueError("subject evidence manifest schema is invalid")
    _trusted, trusted_raw = core._trusted_p0a_contract(_CONTRACT_VERSION)
    contract_hash = _digest(contract_raw)
    if (contract_raw != trusted_raw or contract.get("registration_contract_version") != _CONTRACT_VERSION
            or evidence.get("registration_contract_version") != _CONTRACT_VERSION
            or evidence.get("registration_contract_sha256") != contract_hash
            or evidence.get("run_id") != manifest.get("run_id")):
        raise ValueError("subject trusted contract binding is invalid")
    if (manifest.get("run_status") != "invalid" or manifest.get("sut_returncode") != 0
            or manifest.get("candidate_only") is not True
            or manifest.get("metric_availability") != "unsupported"
            or manifest.get("dds_comm_trace", {}).get("status") != "invalid"
            or "cleanup_error" in manifest or manifest.get("stop_escalated_sigkill") is True
            or "raw/comm_trace/directory_finalization.json" in files):
        raise ValueError("subject runtime finalization state is not the required graceful invalid state")
    nested = validation.get("p0a_evidence")
    if (validation.get("valid_for_metric") is not False
            or validation.get("valid_for_comparison") is not False
            or not isinstance(nested, dict) or nested.get("valid") is not False
            or not {"P0A_ARTIFACT", "P0A_DDS_COMM_TRACE"}.issubset(
                nested.get("failure_codes", []))):
        raise ValueError("subject validation did not fail closed on DDS comm trace")
    if subject_path is None:
        with tempfile.TemporaryDirectory(prefix="mw-perf-runtime-subject-") as temporary:
            subject_path = Path(temporary)
            for relative, data in files.items():
                target = subject_path / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            revalidated = core.validate_dir(subject_path, profile="p0a")
    else:
        revalidated = core.validate_dir(subject_path, profile="p0a")
    if not core._validation_matches_current(validation, revalidated):
        raise ValueError("archived validation does not equal fresh semantic validation")
    revalidated_nested = revalidated.get("p0a_evidence", {})
    if (revalidated.get("valid_for_metric") is not False
            or revalidated.get("valid_for_comparison") is not False
            or not {"P0A_ARTIFACT", "P0A_DDS_COMM_TRACE"}.issubset(
                revalidated_nested.get("failure_codes", []))):
        raise ValueError("subject semantic revalidation did not preserve DDS and artifact failures")
    error = manifest.get("dds_comm_trace", {}).get("error")
    failure_kind, expected_error = _EXPECTED_ERRORS[case_id]
    if not isinstance(error, str) or expected_error not in error:
        raise ValueError("subject collector error does not match its runtime fault case")
    observation = _producer_observation(case_id, files, manifest, evidence)
    declarations = resolution.get("declarations")
    if not isinstance(declarations, list):
        raise ValueError("subject resolution declarations are malformed")
    _validate_runtime_resolution(files, manifest["run_id"], declarations)
    fault_build_identity, runtime_image_hashes = _build_identity(resolution)
    codes = revalidated.get("failure_codes")
    nested_codes = revalidated_nested.get("failure_codes")
    if (not isinstance(codes, list) or not isinstance(nested_codes, list)
            or any(not isinstance(code, str) for code in codes + nested_codes)):
        raise ValueError("subject validation failure codes are malformed")
    return {
        "case_id": case_id,
        "subject_run_id": manifest["run_id"],
        "root_manifest_sha256": _digest(manifest_raw),
        "evidence_manifest_sha256": _digest(evidence_raw),
        "contract": {"id": _CONTRACT_VERSION, "sha256": contract_hash},
        "subject_status": "invalid",
        "sut_exit_zero": True,
        "directory_finalization_present": False,
        "injected_observation": observation,
        "observed_failure_kind": failure_kind,
        "observed_error": expected_error,
        "observed_failure_codes": sorted(set(codes + nested_codes)),
        "fault_build_identity_sha256": fault_build_identity,
        "runtime_image_hashes": runtime_image_hashes,
    }


def _inventory(matrix_id: str, cases: list[dict[str, Any]], trees: Mapping[str, str]) -> dict[str, Any]:
    return {
        "schema": "mw_perf.p0a.runtime_matrix_evidence_manifest.v1",
        "matrix_id": matrix_id,
        "subjects": [{
            "case_id": item["case_id"],
            "subject_run_id": item["subject_run_id"],
            "root_manifest_sha256": item["root_manifest_sha256"],
            "evidence_manifest_sha256": item["evidence_manifest_sha256"],
            "subject_archive_tree_sha256": trees[item["case_id"]],
            "production_build_identity_sha256": item["production_build_identity_sha256"],
            "fault_build_identity_sha256": item["fault_build_identity_sha256"],
            "runtime_image_hashes": item["runtime_image_hashes"],
        } for item in cases],
    }


def _report(matrix_id: str, cases: list[dict[str, Any]], subjects_tree: str,
            anchor: dict[str, Any]) -> dict[str, Any]:
    identities = {item["fault_build_identity_sha256"] for item in cases}
    image_hashes = {_canonical(item["runtime_image_hashes"]) for item in cases}
    if len(identities) != 1 or len(image_hashes) != 1:
        raise ValueError("runtime fault subjects do not share one fault build identity")
    production_identity = anchor["production_build_identity_sha256"]
    fault_identity = next(iter(identities))
    if production_identity == fault_identity:
        raise ValueError("production and fault build identities must differ")
    if any(item.get("production_build_identity_sha256") != production_identity
           for item in cases):
        raise ValueError("runtime fault cases do not bind the production build identity")
    return {
        "schema": _SCHEMA,
        "campaign_classification": {
            "classification": "candidate", "metric_availability": "unsupported",
            "valid_for_comparison": False, "failure_code": "P0A_ARTIFACT",
        },
        "threat_model": "managed_runtime_fault_injection_matrix",
        "evidence_claim": _EVIDENCE_CLAIM,
        "instrumentation_relationship": _INSTRUMENTATION_RELATIONSHIP,
        "production_build_identity_sha256": production_identity,
        "fault_build_identity_sha256": fault_identity,
        "finalized": True,
        "conclusion": "pass",
        "matrix_id": matrix_id,
        "cases": cases,
        "subjects_archive_tree_sha256": subjects_tree,
        "anchor": {key: anchor[key] for key in (
            "run_id", "root_manifest_sha256", "evidence_manifest_sha256", "contract",
            "archive_tree_sha256")},
    }


def _cleanup(experiment: Path, original: BaseException) -> None:
    try:
        experiment.joinpath("payload", _REPORT_NAME).unlink(missing_ok=True)
        shutil.rmtree(experiment)
    except Exception:
        raise RuntimeError("runtime fault evaluation cleanup failed") from original


def evaluate_runtime_fault_matrix(
    subjects: dict[str, Path], experiment_root: Path, production_anchor: Path,
) -> dict[str, Any]:
    """Snapshot and prove the exact five managed runtime fault outcomes."""
    if not isinstance(subjects, dict) or tuple(sorted(subjects)) != CASE_IDS:
        raise ValueError("subjects must contain the exact five sorted runtime fault case IDs")
    if not isinstance(production_anchor, Path):
        raise ValueError("production_anchor must be a real anchor Path")
    experiment = Path(experiment_root)
    created = False
    source_fds: list[int] = []
    sources: list[tuple[int, Path, dict[str, Any]]] = []
    try:
        os.mkdir(experiment, 0o700)
        created = True
        payload = experiment / "payload"
        subject_root = payload / "subjects"
        anchor_root = payload / "anchor"
        os.mkdir(payload, 0o700)
        os.mkdir(subject_root, 0o700)
        os.mkdir(anchor_root, 0o700)
        anchor_fd = os.open(production_anchor, _DIRECTORY_FLAGS)
        source_fds.append(anchor_fd)
        anchor_info = os.fstat(anchor_fd)
        anchor_path_info = os.stat(production_anchor, follow_symlinks=False)
        if _identity(anchor_info) != _identity(anchor_path_info):
            raise ValueError("production anchor must be a stable real directory")
        anchor_initial = {"": ("directory", _identity(anchor_info)), **_scan(anchor_fd)}
        if not _no_seal(anchor_initial):
            raise ValueError("production anchor contains a final campaign seal")
        anchor_destination_fd = os.open(anchor_root, _DIRECTORY_FLAGS)
        try:
            anchor_copied = {"": ("directory", _identity(anchor_info)),
                             **_copy_tree(anchor_fd, anchor_destination_fd)}
        finally:
            os.close(anchor_destination_fd)
        if (anchor_copied != anchor_initial
                or _source_state(anchor_fd, production_anchor) != anchor_initial):
            raise ValueError("production anchor changed while snapshot was copied")
        sources.append((anchor_fd, production_anchor, anchor_initial))
        anchor_snapshot = capture_payload_snapshot(anchor_root)
        anchor = _analyze_anchor(dict(anchor_snapshot.files), anchor_path=anchor_root)
        anchor["archive_tree_sha256"] = compute_snapshot_archive_tree(anchor_snapshot)["sha256"]
        if _source_state(anchor_fd, production_anchor) != anchor_initial:
            raise ValueError("production anchor changed during semantic evaluation")
        cases: list[dict[str, Any]] = []
        trees: dict[str, str] = {}
        for case_id in CASE_IDS:
            source = Path(subjects[case_id])
            destination = subject_root / case_id
            os.mkdir(destination, 0o700)
            source_fd = os.open(source, _DIRECTORY_FLAGS)
            source_fds.append(source_fd)
            source_info = os.fstat(source_fd)
            path_info = os.stat(source, follow_symlinks=False)
            if _identity(source_info) != _identity(path_info):
                raise ValueError("subject must be a stable real directory")
            initial = {"": ("directory", _identity(source_info)), **_scan(source_fd)}
            if not _no_seal(initial):
                raise ValueError("subject contains a final campaign seal")
            destination_fd = os.open(destination, _DIRECTORY_FLAGS)
            try:
                copied = {"": ("directory", _identity(source_info)),
                          **_copy_tree(source_fd, destination_fd)}
            finally:
                os.close(destination_fd)
            if copied != initial or _source_state(source_fd, source) != initial:
                raise ValueError("subject changed while snapshot was copied")
            sources.append((source_fd, source, initial))
            snapshot = capture_payload_snapshot(destination)
            subject_files = dict(snapshot.files)
            case = _analyze_subject(case_id, subject_files, subject_path=destination)
            case["production_build_identity_sha256"] = \
                anchor["production_build_identity_sha256"]
            cases.append(case)
            trees[case_id] = compute_snapshot_archive_tree(snapshot)["sha256"]
            if _source_state(source_fd, source) != initial:
                raise ValueError("subject changed during semantic evaluation")
        provisional = {"schema": "mw_perf.p0a.runtime_matrix_seed.v1",
                       "anchor": {key: anchor[key] for key in (
                           "run_id", "root_manifest_sha256", "evidence_manifest_sha256",
                           "contract", "archive_tree_sha256")},
                       "subjects": [{"case_id": case["case_id"],
                                     "subject_run_id": case["subject_run_id"],
                                     "root_manifest_sha256": case["root_manifest_sha256"],
                                     "evidence_manifest_sha256": case["evidence_manifest_sha256"],
                                     "subject_archive_tree_sha256": trees[case["case_id"]]}
                                    for case in cases]}
        matrix_id = "runtime-fault-matrix-" + _digest(_canonical(provisional))[:24]
        inventory = _inventory(matrix_id, cases, trees)
        inventory_raw = _canonical(inventory)
        if core._schema_errors(inventory, _schema("runtime-matrix-evidence-manifest.schema.v1.json")):
            raise RuntimeError("runtime fault evaluator produced an invalid subject inventory")
        subjects_tree = _digest(_canonical({
            "anchor_archive_tree_sha256": anchor["archive_tree_sha256"],
            "subjects_archive_tree_sha256": compute_archive_tree(subject_root)["sha256"],
        }))
        report = _report(matrix_id, cases, subjects_tree, anchor)
        if core._schema_errors(report, _schema("fault-runtime-matrix-result.schema.v1.json")):
            raise RuntimeError("runtime fault evaluator produced an invalid report")
        report_raw = _canonical(report)
        matrix_manifest = {
            "schema": "mw_perf.p0a.runtime_matrix_manifest.v1", "matrix_id": matrix_id,
            "report_sha256": _digest(report_raw),
            "evidence_manifest_sha256": _digest(inventory_raw),
        }
        if core._schema_errors(matrix_manifest, _schema("runtime-matrix-manifest.schema.v1.json")):
            raise RuntimeError("runtime fault evaluator produced an invalid matrix manifest")
        for name, data in ((_REPORT_NAME, report_raw), (_MATRIX_MANIFEST, _canonical(matrix_manifest)),
                           (_MATRIX_EVIDENCE, inventory_raw)):
            fd = os.open(payload / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                         | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0), 0o600)
            try:
                view = memoryview(data)
                while view:
                    written = os.write(fd, view)
                    if written <= 0:
                        raise OSError("report write made no progress")
                    view = view[written:]
                os.fsync(fd)
            finally:
                os.close(fd)
        for source_fd, source, initial in sources:
            if _source_state(source_fd, source) != initial:
                raise ValueError("anchor or subject changed before matrix publication completed")
        return report
    except FileExistsError as exc:
        raise ValueError("experiment_root must not already exist") from exc
    except BaseException as exc:
        if created:
            _cleanup(experiment, exc)
        if isinstance(exc, (ValueError, RuntimeError)):
            raise
        if isinstance(exc, Exception):
            raise ValueError("runtime fault evaluation failed") from exc
        raise
    finally:
        for fd in source_fds:
            os.close(fd)


def _validate_payload_snapshot(snapshot: PayloadSnapshot, report: dict[str, Any]) -> tuple[dict[str, Any], bytes, bytes]:
    files = snapshot.files
    expected_top = {_REPORT_NAME, _MATRIX_MANIFEST, _MATRIX_EVIDENCE}
    if (not expected_top.issubset(files)
            or any(name not in expected_top and not name.startswith("subjects/")
                   and not name.startswith("anchor/") for name in files)
            or any(name.startswith("subjects/") and name.split("/", 2)[1] not in CASE_IDS
                   for name in files)):
        raise ValueError("runtime matrix payload has an unexpected member")
    stored = _strict_object(files[_REPORT_NAME], "runtime matrix report")
    if (stored != report or files[_REPORT_NAME] != _canonical(stored)
            or core._schema_errors(stored, _schema("fault-runtime-matrix-result.schema.v1.json"))):
        raise ValueError("runtime matrix report is not canonical and complete")
    cases = [_analyze_subject(case_id, _files_for_prefix(files, f"subjects/{case_id}"),
                              subject_path=None) for case_id in CASE_IDS]
    anchor = _analyze_anchor(_files_for_prefix(files, "anchor"), anchor_path=None)
    anchor["archive_tree_sha256"] = compute_snapshot_archive_tree(snapshot, "anchor")["sha256"]
    for case in cases:
        case["production_build_identity_sha256"] = anchor[
            "production_build_identity_sha256"]
    trees = {case_id: compute_snapshot_archive_tree(snapshot, f"subjects/{case_id}")["sha256"]
             for case_id in CASE_IDS}
    inventory = _inventory(stored["matrix_id"], cases, trees)
    inventory_raw = _canonical(inventory)
    if (core._schema_errors(inventory, _schema("runtime-matrix-evidence-manifest.schema.v1.json"))
            or files[_MATRIX_EVIDENCE] != inventory_raw):
        raise ValueError("runtime matrix subject inventory does not bind the payload")
    subjects_tree = _digest(_canonical({
        "anchor_archive_tree_sha256": anchor["archive_tree_sha256"],
        "subjects_archive_tree_sha256": compute_snapshot_archive_tree(
            snapshot, "subjects")["sha256"],
    }))
    if stored != _report(stored["matrix_id"], cases, subjects_tree, anchor):
        raise ValueError("runtime matrix report does not match revalidated subjects")
    matrix_manifest = {
        "schema": "mw_perf.p0a.runtime_matrix_manifest.v1", "matrix_id": stored["matrix_id"],
        "report_sha256": _digest(files[_REPORT_NAME]),
        "evidence_manifest_sha256": _digest(inventory_raw),
    }
    if (core._schema_errors(matrix_manifest, _schema("runtime-matrix-manifest.schema.v1.json"))
            or files[_MATRIX_MANIFEST] != _canonical(matrix_manifest)):
        raise ValueError("runtime matrix manifest does not bind report and inventory")
    return stored, files[_REPORT_NAME], inventory_raw


def build_runtime_fault_companion(
    report: dict[str, Any], experiment_root: Path, *, campaign_id: str,
    companion_id: str, object_store: Path,
) -> dict[str, Any]:
    """Semantically revalidate and package one immutable matrix payload snapshot."""
    snapshot = capture_payload_snapshot(Path(experiment_root) / "payload")
    stored, report_raw, inventory_raw = _validate_payload_snapshot(snapshot, report)
    _trusted, contract_raw = core._trusted_p0a_contract(_CONTRACT_VERSION)
    metadata = {
        "campaign_id": campaign_id,
        "role": "fault_runtime_matrix",
        "companion_id": companion_id,
        "run_id": stored["matrix_id"],
        "evidence_contract": {"id": _CONTRACT_VERSION, "sha256": _digest(contract_raw)},
        "root_manifest_sha256": _digest(report_raw),
        "evidence_manifest_sha256": _digest(inventory_raw),
        "build_identity_sha256": stored["fault_build_identity_sha256"],
    }
    descriptor = build_companion_object_from_snapshot(snapshot, Path(object_store), metadata)
    digest = descriptor["object_sha256"]
    object_path = Path(object_store) / "sha256" / digest[:2] / f"{digest}.tar"
    if not validate_runtime_fault_companion(object_path, descriptor)["valid"]:
        raise ValueError("built runtime matrix companion failed specialized validation")
    return descriptor


def validate_runtime_fault_companion(
    object_path: Path, expected_descriptor: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Validate an immutable runtime matrix companion entirely from archived bytes."""
    if expected_descriptor is not None and not isinstance(expected_descriptor, dict):
        raise TypeError("expected_descriptor must be an object or None")
    generic = validate_companion_object(Path(object_path), expected_descriptor)
    descriptor = generic.get("descriptor")
    codes = set(generic.get("failure_codes", []))
    if generic.get("valid") is not True:
        return {"valid": False, "failure_codes": sorted(codes), "descriptor": descriptor}
    if expected_descriptor is None:
        return {"valid": False, "failure_codes": ["RUNTIME_EXPECTED_DESCRIPTOR"],
                "descriptor": descriptor}
    try:
        if not isinstance(descriptor, dict):
            raise ValueError("runtime companion descriptor is unavailable")
        snapshot, bundle_manifest, raw = _archived_payload(Path(object_path))
        if _digest(raw) != descriptor.get("object_sha256"):
            raise ValueError("runtime companion object hash is invalid")
        report = _strict_object(snapshot.files[_REPORT_NAME], "runtime matrix report")
        stored, report_raw, inventory_raw = _validate_payload_snapshot(snapshot, report)
        _trusted, contract_raw = core._trusted_p0a_contract(_CONTRACT_VERSION)
        bindings = {
            "role": "fault_runtime_matrix",
            "run_id": stored["matrix_id"],
            "root_manifest_sha256": _digest(report_raw),
            "evidence_manifest_sha256": _digest(inventory_raw),
            "build_identity_sha256": stored["fault_build_identity_sha256"],
            "evidence_contract": {"id": _CONTRACT_VERSION,
                                  "sha256": _digest(contract_raw)},
        }
        if any(bundle_manifest.get(key) != value for key, value in bindings.items()):
            raise ValueError("runtime companion descriptor bindings are invalid")
        case_ids = [item.get("case_id") for item in stored.get("cases", [])]
        observations = [item.get("injected_observation") for item in stored.get("cases", [])]
        if (case_ids != list(CASE_IDS) or len(set(case_ids)) != len(CASE_IDS)
                or len(observations) != len(CASE_IDS)
                or stored.get("campaign_classification") != {
                    "classification": "candidate", "metric_availability": "unsupported",
                    "valid_for_comparison": False, "failure_code": "P0A_ARTIFACT"}
                or stored.get("production_build_identity_sha256") ==
                   stored.get("fault_build_identity_sha256")):
            raise ValueError("runtime matrix acceptance invariants are invalid")
    except (KeyError, OSError, tarfile.TarError, EOFError, ValueError, TypeError,
            json.JSONDecodeError, UnicodeError):
        codes.add("RUNTIME_FAULT_PAYLOAD")
        return {"valid": False, "failure_codes": sorted(codes), "descriptor": descriptor}
    return {
        "valid": True,
        "failure_codes": [],
        "descriptor": descriptor,
        "anchor": stored["anchor"],
        "production_build_identity_sha256": stored["production_build_identity_sha256"],
        "fault_build_identity_sha256": stored["fault_build_identity_sha256"],
    }
