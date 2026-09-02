"""Two-stage v9 formal-anchor materialization."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any, Mapping

from . import campaign, core
from .campaign import PayloadSnapshot, build_companion_object_from_snapshot
from .json_strict import loads as strict_json_loads

_ROLES = ("fault_evidence_tamper", "fault_runtime_matrix", "rollback_trace_off")
_FORMAL = "validation/formal_anchor.json"
_TRACE = "validation/trace_quality.json"
_EVIDENCE = "capability/p0a_evidence_manifest.json"
_CONTRACT = "registration/contract.json"
_FORMAL_AXIS_RULES = {"candidate_only", "metric_available", "p0a_evidence"}
_KNOWN_VALIDATION_RULES = (
    "resource_integrity", "required_fields", "manifest_schema", "sample_artifacts",
    "candidate_only", "run_completed", "samples_present", "metrics_valid",
    "adapter_ready", "metric_available", "scenario_contract", "ibmw_provenance",
    "runtime_images", "core_metrics_required", "latency_artifact_required",
    "collector_managed_comparison", "comparison_metadata", "comparison_resources",
    "comparison_latency_csv", "p0a_profile_marker", "p0a_evidence",
    "dds_comm_trace_summary")
_SOURCE_FAILURE_CODES = ["CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"]
_FORMAL_SUPPORT_FILES = frozenset({
    "manifest.json", "validation.json", _EVIDENCE,
    "registration/contract.json", "registration/resolution_declarations.json",
    "environment/host.json", "logs/sut.stderr.log", "logs/sut.stdout.log",
    "raw/network.jsonl", "raw/process.jsonl", "report.md",
})


def _canonical(value: Any, *, newline: bool = True) -> bytes:
    return core._canonical_json_bytes(value, newline=newline)


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _object_path(store: Path, descriptor: Mapping[str, Any]) -> Path:
    digest = descriptor.get("object_sha256")
    if not isinstance(digest, str):
        raise ValueError("descriptor object hash is invalid")
    return campaign._checked_object_path(Path(store), digest)


def _descriptor(value: Any, role: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != campaign._DESCRIPTOR_KEYS:
        raise ValueError(f"{role} descriptor is not a full generic descriptor")
    if value.get("role") != role:
        raise ValueError(f"descriptor role mismatch for {role}")
    return value


def _read_payload(path: Path, descriptor: dict[str, Any]) -> dict[str, bytes]:
    generic = campaign.validate_companion_object(path, descriptor)
    if not generic.get("valid"):
        raise ValueError("companion object failed generic validation")
    return campaign._read_canonical_payload_object(path)


def _payload_json(payloads: Mapping[str, bytes], name: str) -> dict[str, Any]:
    value = strict_json_loads(payloads[name])
    if not isinstance(value, dict):
        raise ValueError(f"{name} is not a JSON object")
    return value


def _expected_formal_payloads(evidence: Mapping[str, Any]) -> set[str]:
    entries = evidence.get("entries")
    if not isinstance(entries, list):
        raise ValueError("formal evidence entries are invalid")
    expected = {
        item["relative_path"] for item in entries
        if isinstance(item, dict) and item.get("status") == "required"
        and item.get("finalization") == "complete"
    }
    if len(expected) != sum(
            1 for item in entries
            if isinstance(item, dict) and item.get("status") == "required"
            and item.get("finalization") == "complete"):
        raise ValueError("formal evidence paths are invalid")
    return expected | _FORMAL_SUPPORT_FILES


def _snapshot_regular_files(root: Path) -> dict[str, bytes]:
    if root.is_symlink() or not root.is_dir():
        raise ValueError("formal snapshot root is not a directory")
    files: dict[str, bytes] = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise ValueError("formal snapshot contains a symlink")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError("formal snapshot contains an unsupported node")
        files[relative] = path.read_bytes()
    return files


def _write_json(root: Path, relative: str, value: Any) -> None:
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(_canonical(value))


def _formal_doc(schema: str, run_id: str, formal_hash: str,
                source: dict[str, Any], evidence_hash: str,
                **bindings: Any) -> dict[str, Any]:
    value: dict[str, Any] = {"schema": schema, "run_id": run_id,
        "formal_anchor_sha256": formal_hash, "source_anchor": source,
        "source_evidence_manifest_sha256": evidence_hash, "finalized": True,
        "status": {"valid_for_p0a_completion": True,
                    "metric_availability": "unsupported",
                    "valid_for_comparison": False}, "conclusion": "pass"}
    value.update(bindings)
    return value


def _formalize_validation(source: Mapping[str, Any]) -> dict[str, Any]:
    """Apply the closed candidate-to-formal completion-axis transition."""
    if not isinstance(source, Mapping):
        raise ValueError("stage1 validation is unavailable")
    value = dict(source)
    nested_source = value.get("p0a_evidence")
    if not isinstance(nested_source, Mapping):
        raise ValueError("stage1 P0a validation is unavailable")
    top_codes = value.get("failure_codes")
    nested_codes = nested_source.get("failure_codes")
    if (top_codes != _SOURCE_FAILURE_CODES
            or nested_codes != ["P0A_ARTIFACT"]):
        raise ValueError("stage1 has failures outside the formalizable axis")
    rules = value.get("rules")
    if not isinstance(rules, list) or any(not isinstance(item, dict) for item in rules):
        raise ValueError("stage1 validation rules are unavailable")
    by_name = {item.get("name"): item for item in rules}
    if (len(by_name) != len(rules)
            or tuple(item.get("name") for item in rules) != _KNOWN_VALIDATION_RULES
            or any(item.get("passed") is not False for name, item in by_name.items()
                   if name in _FORMAL_AXIS_RULES)
            or any(item.get("passed") is not True for name, item in by_name.items()
                   if name not in _FORMAL_AXIS_RULES)):
        raise ValueError("stage1 validation has an unexplained failed rule")
    formal_rules = [dict(item) for item in rules]
    for item in formal_rules:
        if item.get("name") in {"candidate_only", "p0a_evidence"}:
            item["passed"] = True
    nested = dict(nested_source)
    nested["valid"] = True
    nested["failure_codes"] = []
    value.update({"validation_profile": "p0a", "valid_for_p0a_completion": True,
                  "valid_for_collection": True, "valid_for_metric": False,
                  "valid_for_comparison": False, "failure_codes": ["METRIC_AVAILABLE"],
                  "rules": formal_rules, "p0a_evidence": nested})
    return value


def _validate_formal_validation(value: Any) -> None:
    if not isinstance(value, dict):
        raise ValueError("formal validation is unavailable")
    nested = value.get("p0a_evidence")
    rules = value.get("rules")
    if (value.get("validation_profile") != "p0a"
            or value.get("valid_for_p0a_completion") is not True
            or value.get("valid_for_collection") is not True
            or value.get("valid_for_metric") is not False
            or value.get("valid_for_comparison") is not False
            or value.get("failure_codes") != ["METRIC_AVAILABLE"]
            or not isinstance(nested, dict) or nested.get("valid") is not True
            or nested.get("failure_codes") != []
            or not isinstance(rules, list) or any(not isinstance(item, dict) for item in rules)):
        raise ValueError("formal validation status is invalid")
    by_name = {item.get("name"): item for item in rules}
    if (len(by_name) != len(rules)
            or by_name.get("candidate_only", {}).get("passed") is not True
            or by_name.get("p0a_evidence", {}).get("passed") is not True
            or tuple(item.get("name") for item in rules) != _KNOWN_VALIDATION_RULES
            or any(by_name.get(name, {}).get("passed") is not True
                   for name in ("candidate_only", "p0a_evidence"))
            or by_name.get("metric_available", {}).get("passed") is not False
            or any(item.get("passed") is not True for name, item in by_name.items()
                   if name not in _FORMAL_AXIS_RULES)):
        raise ValueError("formal validation rules are invalid")
    if "P0A_ARTIFACT" in json.dumps(value, sort_keys=True):
        raise ValueError("formal validation retains P0A_ARTIFACT")


def validate_formal_anchor_snapshot(path: Path) -> dict[str, Any]:
    """Validate stage-2 semantic status and all one-way formal bindings."""
    root = Path(path)
    try:
        files = _snapshot_regular_files(root)
        if any(campaign._is_final_campaign_seal(name) for name in files):
            raise ValueError("formal snapshot contains a campaign seal")
        formal = strict_json_loads(files[_FORMAL]); evidence = strict_json_loads(files[_EVIDENCE])
        if formal["schema"] != "mw_perf.formal_anchor.v1" or formal["finalized"] is not True:
            raise ValueError("formal anchor is not finalized")
        formal_raw = files[_FORMAL]
        formal_schema = strict_json_loads((Path(__file__).parent / "schemas" /
                                    "formal-anchor.schema.v1.json").read_bytes())
        if core._schema_errors(formal, formal_schema) or formal_raw != _canonical(formal):
            raise ValueError("formal anchor schema or canonical encoding is invalid")
        expected_id = dict(formal); expected_id.pop("formal_anchor_id")
        if formal["formal_anchor_id"] != _sha(_canonical(expected_id, newline=False)):
            raise ValueError("formal anchor id is not content-derived")
        formal_hash = _sha(formal_raw)
        trusted_raw = campaign._validate_trusted_evidence_payload(
            files, evidence, expected_version="mw_perf.p0a_contract.v9",
            expected_run_type="formal_p0a", expected_run_id=formal["run_id"])
        if set(files) != _expected_formal_payloads(evidence):
            raise ValueError("formal snapshot file set is invalid")
        if formal.get("registration_contract") != {
                "version": "mw_perf.p0a_contract.v9",
                "sha256": _sha(trusted_raw)}:
            raise ValueError("formal anchor contract binding is invalid")
        validation = strict_json_loads(files["validation.json"])
        _validate_formal_validation(validation)
        schema_names = {
            "validation/fault_injection.json": "fault-injection-formal.schema.v2.json",
            "validation/rollback.json": "rollback-formal.schema.v2.json",
            _TRACE: "trace-quality-formal.schema.v2.json"}
        for name, schema_name in schema_names.items():
            doc = strict_json_loads(files[name])
            schema = strict_json_loads((Path(__file__).parent / "schemas" / schema_name).read_bytes())
            if core._schema_errors(doc, schema) or files[name] != _canonical(doc):
                raise ValueError("formal artifact schema or canonical encoding is invalid")
            expected_bindings: dict[str, Any] = {}
            if name == "validation/fault_injection.json":
                expected_bindings["companions"] = {
                    role: formal["companions"][role]
                    for role in ("fault_evidence_tamper", "fault_runtime_matrix")}
            elif name == "validation/rollback.json":
                expected_bindings["companion"] = formal["companions"]["rollback_trace_off"]
            else:
                expected_bindings["source_trace_quality_content_sha256"] = (
                    formal["source_trace_quality_content_sha256"])
            if (doc.get("formal_anchor_sha256") != formal_hash
                    or doc.get("run_id") != formal.get("run_id")
                    or doc.get("source_anchor") != formal.get("source_anchor")
                    or any(doc.get(key) != value for key, value in expected_bindings.items())
                    or doc.get("source_evidence_manifest_sha256") !=
                        formal["source_anchor"]["evidence_manifest_sha256"]
                    or doc.get("finalized") is not True
                    or doc.get("status") != formal.get("status")
                    or doc.get("conclusion") != "pass"):
                raise ValueError("formal artifact binding is invalid")
        return {"valid": True, "formal_anchor_sha256": formal_hash,
                "formal_anchor_id": formal["formal_anchor_id"]}
    except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise ValueError("invalid formal anchor snapshot") from exc


def validate_formal_archive_payload(payloads: Mapping[str, bytes], *,
                                    formal_descriptor: Mapping[str, Any],
                                    source_descriptor: Mapping[str, Any],
                                    companion_descriptors: Mapping[str, Mapping[str, Any]]) -> dict[str, Any]:
    """Validate the semantic payload of a v9 formal companion archive.

    This operates on immutable payload bytes supplied by the campaign object
    validator.  It deliberately does not hash or rebuild the formal object.
    """
    try:
        required = {_FORMAL, "manifest.json", "validation.json", _EVIDENCE,
                    "registration/contract.json", _TRACE,
                    "validation/fault_injection.json", "validation/rollback.json"}
        if any(campaign._is_final_campaign_seal(name) for name in payloads):
            raise ValueError("formal archive contains a campaign seal")
        if not required.issubset(payloads):
            raise ValueError("formal archive file set is invalid")
        formal = _payload_json(payloads, _FORMAL)
        manifest = _payload_json(payloads, "manifest.json")
        validation = _payload_json(payloads, "validation.json")
        evidence = _payload_json(payloads, _EVIDENCE)
        contract_raw = payloads[_CONTRACT]
        _payload_json(payloads, "registration/contract.json")
        formal_descriptor = _descriptor(formal_descriptor, "exact_anchor")
        source_descriptor = _descriptor(source_descriptor, "exact_anchor")
        if formal_descriptor["evidence_contract"]["id"] != "mw_perf.p0a_contract.v9":
            raise ValueError("formal outer descriptor contract version is invalid")
        if formal.get("schema") != "mw_perf.formal_anchor.v1" or formal.get("finalized") is not True:
            raise ValueError("formal archive status is invalid")
        schema_dir = Path(__file__).parent / "schemas"
        formal_schema = strict_json_loads((schema_dir / "formal-anchor.schema.v1.json").read_bytes())
        formal_raw = payloads[_FORMAL]
        if core._schema_errors(formal, formal_schema) or formal_raw != _canonical(formal):
            raise ValueError("formal archive schema or canonical encoding is invalid")
        formal_id_input = dict(formal)
        formal_id_input.pop("formal_anchor_id", None)
        if formal.get("formal_anchor_id") != _sha(_canonical(formal_id_input, newline=False)):
            raise ValueError("formal archive id is invalid")
        if formal.get("source_anchor") != dict(source_descriptor):
            raise ValueError("formal archive source descriptor mismatch")
        expected_companions = {
            role: _descriptor(companion_descriptors[role], role) for role in _ROLES}
        if formal.get("companions") != expected_companions:
            raise ValueError("formal archive companion descriptors mismatch")
        if (formal.get("run_id") != formal_descriptor.get("run_id")
                or formal.get("run_id") != source_descriptor.get("run_id")
                or formal_descriptor.get("campaign_id") != source_descriptor.get("campaign_id")
                or any(item.get("campaign_id") != formal_descriptor.get("campaign_id")
                       for item in expected_companions.values())
                or formal_descriptor.get("build_identity_sha256") !=
                    source_descriptor.get("build_identity_sha256")):
            raise ValueError("formal archive run binding is invalid")
        trusted_raw = campaign._validate_trusted_evidence_payload(
            payloads, evidence, expected_version="mw_perf.p0a_contract.v9",
            expected_run_type="formal_p0a", expected_run_id=formal["run_id"],
            allowed_incomplete_paths=frozenset())
        if set(payloads) != _expected_formal_payloads(evidence):
            raise ValueError("formal archive file set is invalid")
        if (contract_raw != trusted_raw
                or formal_descriptor.get("evidence_contract") != {
                    "id": "mw_perf.p0a_contract.v9", "sha256": _sha(trusted_raw)}
                or formal_descriptor.get("root_manifest_sha256") != _sha(payloads["manifest.json"])
                or formal_descriptor.get("evidence_manifest_sha256") != _sha(payloads[_EVIDENCE])):
            raise ValueError("formal outer descriptor binding is invalid")
        if formal.get("registration_contract") != {
                "version": "mw_perf.p0a_contract.v9", "sha256": _sha(contract_raw)}:
            raise ValueError("formal archive contract binding is invalid")
        status = formal.get("status")
        if status != {"valid_for_p0a_completion": True,
                      "metric_availability": "unsupported",
                      "valid_for_comparison": False}:
            raise ValueError("formal archive classification is invalid")
        if (manifest.get("candidate_only") is not False
                or manifest.get("metric_availability") != "unsupported"
                or manifest.get("valid_for_comparison") is not False
                or manifest.get("run_id") != formal.get("run_id")
                or manifest.get("formalization") != {
                    "source_anchor": dict(source_descriptor),
                    "formal_anchor_id": formal["formal_anchor_id"],
                    "contract_version": "mw_perf.p0a_contract.v9"}):
            raise ValueError("formal archive status is invalid")
        _validate_formal_validation(validation)
        formal_hash = _sha(_canonical(formal))
        for name in ("validation/fault_injection.json", "validation/rollback.json", _TRACE):
            document = _payload_json(payloads, name)
            schema_name = {"validation/fault_injection.json": "fault-injection-formal.schema.v2.json",
                           "validation/rollback.json": "rollback-formal.schema.v2.json",
                           _TRACE: "trace-quality-formal.schema.v2.json"}[name]
            document_raw = payloads[name]
            document_schema = strict_json_loads((schema_dir / schema_name).read_bytes())
            if core._schema_errors(document, document_schema) or document_raw != _canonical(document):
                raise ValueError("formal artifact schema or canonical encoding is invalid")
            if (document.get("formal_anchor_sha256") != formal_hash
                    or document.get("run_id") != formal.get("run_id")
                    or document.get("source_anchor") != dict(source_descriptor)
                    or document.get("source_evidence_manifest_sha256") !=
                        source_descriptor["evidence_manifest_sha256"]
                    or document.get("finalized") is not True
                    or document.get("status") != status
                    or document.get("conclusion") != "pass"):
                raise ValueError("formal artifact binding is invalid")
            if (name == "validation/fault_injection.json" and document.get("companions") != {
                    role: expected_companions[role]
                    for role in ("fault_evidence_tamper", "fault_runtime_matrix")}
                    or name == "validation/rollback.json" and
                    document.get("companion") != expected_companions["rollback_trace_off"]
                    or name == _TRACE and document.get(
                        "source_trace_quality_content_sha256") !=
                        formal["source_trace_quality_content_sha256"]):
                raise ValueError("formal artifact companion binding is invalid")
        return {"valid": True, "failure_codes": [], "descriptor": dict(formal_descriptor)}
    except (KeyError, TypeError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid formal archive payload") from exc


def validate_formal_exact_anchor_companion(
        object_path: Path, expected_descriptor: dict[str, Any] | None = None,
        *, object_store: Path | None = None,
        expected_source_descriptor: Mapping[str, Any] | None = None,
        expected_companion_descriptors: Mapping[str, Mapping[str, Any]] | None = None
        ) -> dict[str, Any]:
    """Validate a v9 exact-anchor object and its embedded v8 closure."""
    if expected_descriptor is None:
        return {"valid": False, "failure_codes": ["FORMAL_EXPECTED_DESCRIPTOR"], "descriptor": None}
    generic = campaign.validate_companion_object(Path(object_path), expected_descriptor)
    if not generic.get("valid"):
        return {"valid": False, "failure_codes": generic.get("failure_codes", []),
                "descriptor": generic.get("descriptor")}
    try:
        payloads = campaign._read_canonical_payload_object(Path(object_path))
        formal = _payload_json(payloads, _FORMAL)
        source = _descriptor(formal.get("source_anchor"), "exact_anchor")
        companions = {role: _descriptor(formal["companions"].get(role), role) for role in _ROLES}
        if (expected_source_descriptor is not None
                and source != dict(expected_source_descriptor)):
            raise ValueError("formal source closure mismatch")
        if expected_companion_descriptors is not None:
            expected_companions = {
                role: dict(expected_companion_descriptors[role]) for role in _ROLES}
            if companions != expected_companions:
                raise ValueError("formal companion closure mismatch")
        if source["evidence_contract"]["id"] != "mw_perf.p0a_contract.v8":
            raise ValueError("formal source is not v8")
        if object_store is None:
            object_store = Path(object_path).parents[2]
        source_path = campaign._checked_object_path(object_store, source["object_sha256"])
        if not campaign.validate_exact_anchor_companion(source_path, source).get("valid"):
            raise ValueError("embedded v8 source is invalid")
        for role, descriptor in companions.items():
            path = campaign._checked_object_path(object_store, descriptor["object_sha256"])
            if not campaign._specialized_companion(role, path, descriptor).get("valid"):
                raise ValueError(f"embedded {role} is invalid")
        return validate_formal_archive_payload(payloads, formal_descriptor=expected_descriptor,
                                               source_descriptor=source,
                                               companion_descriptors=companions)
    except (KeyError, TypeError, ValueError, OSError, UnicodeError, json.JSONDecodeError):
        return {"valid": False, "failure_codes": ["FORMAL_ARCHIVE"],
                "descriptor": generic.get("descriptor")}


def materialize_formal_anchor(*, stage1_descriptor, stage1_object_path,
                              companion_descriptors, object_store, destination,
                              campaign_id, companion_id) -> dict:
    """Create and atomically publish a v9 formal exact-anchor snapshot."""
    source = _descriptor(stage1_descriptor, "exact_anchor")
    if source["evidence_contract"]["id"] != "mw_perf.p0a_contract.v8":
        raise ValueError("stage1 must be a v8 exact anchor")
    if not isinstance(companion_descriptors, Mapping) or set(companion_descriptors) != set(_ROLES):
        raise ValueError("companion roles must be exact")
    companions = {role: _descriptor(companion_descriptors[role], role) for role in _ROLES}
    store = Path(object_store); destination = Path(destination)
    if destination.exists():
        raise FileExistsError("destination already exists")
    stage1_path = Path(stage1_object_path)
    stage_files = _read_payload(stage1_path, source)
    stage_evidence = strict_json_loads(stage_files[_EVIDENCE])
    if strict_json_loads(stage_files[_TRACE]).get("schema") != "mw_perf.trace_quality.v1":
        raise ValueError("stage1 trace quality must be v1")
    if (stage_evidence.get("run_id") != source["run_id"]
            or stage_evidence.get("registration_contract_version") !=
            "mw_perf.p0a_contract.v8"):
        raise ValueError("stage1 evidence identity is invalid")
    results: dict[str, dict[str, Any]] = {}
    results["exact_anchor"] = campaign.validate_exact_anchor_companion(stage1_path, source)
    for role in _ROLES:
        path = _object_path(store, companions[role])
        result = campaign._specialized_companion(role, path, companions[role])
        if not result.get("valid"):
            raise ValueError(f"{role} companion failed specialized validation")
        results[role] = result
    campaign._validate_role_bindings(results)
    if any(d["campaign_id"] != campaign_id for d in (source, *companions.values())):
        raise ValueError("campaign identity mismatch")
    trusted_contract, contract_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v9")
    run_id = source["run_id"]
    formal = {"schema": "mw_perf.formal_anchor.v1", "formal_anchor_id": "0" * 64,
        "run_id": run_id, "registration_contract": {"version": "mw_perf.p0a_contract.v9", "sha256": _sha(contract_raw)},
        "source_anchor": source, "companions": companions,
        "source_trace_quality_content_sha256": _sha(stage_files[_TRACE]), "finalized": True,
        "status": {"valid_for_p0a_completion": True, "metric_availability": "unsupported", "valid_for_comparison": False}, "conclusion": "pass"}
    formal["formal_anchor_id"] = _sha(_canonical({k: v for k, v in formal.items() if k != "formal_anchor_id"}, newline=False))
    formal_raw = _canonical(formal)
    formal_hash = _sha(formal_raw)
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}.", dir=destination.parent))
    published = False
    try:
        for relative, data in stage_files.items():
            target = staging / relative; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
        (staging / _CONTRACT).write_bytes(contract_raw)
        _write_json(staging, _FORMAL, formal)
        _write_json(staging, "validation/fault_injection.json", _formal_doc(
            "mw_perf.fault_injection_formal.v2", run_id, formal_hash, source,
            source["evidence_manifest_sha256"], companions={
                role: companions[role]
                for role in ("fault_evidence_tamper", "fault_runtime_matrix")}))
        _write_json(staging, "validation/rollback.json", _formal_doc(
            "mw_perf.rollback_formal.v2", run_id, formal_hash, source,
            source["evidence_manifest_sha256"],
            companion=companions["rollback_trace_off"]))
        _write_json(staging, _TRACE, _formal_doc(
            "mw_perf.trace_quality_formal.v2", run_id, formal_hash, source,
            source["evidence_manifest_sha256"],
            source_trace_quality_content_sha256=_sha(stage_files[_TRACE])))
        root_manifest = strict_json_loads((staging / "manifest.json").read_bytes())
        root_manifest.update({"artifact_kind": "formal_anchor_snapshot", "formalization": {"source_anchor": source, "formal_anchor_id": formal["formal_anchor_id"], "contract_version": "mw_perf.p0a_contract.v9"}, "run_type": "formal_p0a", "candidate_only": False, "metric_availability": "unsupported", "valid_for_comparison": False})
        _write_json(staging, "manifest.json", root_manifest)
        validation = _formalize_validation(strict_json_loads(
            (staging / "validation.json").read_bytes()))
        _write_json(staging, "validation.json", validation)
        (staging / _CONTRACT).unlink()
        (staging / _EVIDENCE).unlink()
        config = {"registration_contract_version": "mw_perf.p0a_contract.v9"}
        rebuilt = core.build_p0a_evidence_manifest(staging, config, run_id=run_id,
            capability_id=stage_evidence["capability_id"], path_id=stage_evidence["path_id"],
            deployment_mode="managed", run_type="formal_p0a", observer=False, measurement_kind="exact_correlated")
        if rebuilt is None:
            raise ValueError("could not regenerate formal evidence manifest")
        _write_json(staging, _EVIDENCE, rebuilt)
        validate_formal_anchor_snapshot(staging)
        descriptor = build_companion_object_from_snapshot(campaign.capture_payload_snapshot(staging), store, {
            "campaign_id": campaign_id, "role": "exact_anchor", "companion_id": companion_id,
            "run_id": run_id, "evidence_contract": {"id": "mw_perf.p0a_contract.v9", "sha256": _sha(contract_raw)},
            "root_manifest_sha256": _sha((staging / "manifest.json").read_bytes()), "evidence_manifest_sha256": _sha((staging / _EVIDENCE).read_bytes()),
            "build_identity_sha256": source["build_identity_sha256"]})
        result = {"report": strict_json_loads((staging / "manifest.json").read_bytes()), "formal_descriptor": descriptor,
                  "source_descriptor": source, "formal_anchor_sha256": formal_hash}
        campaign._rename_noreplace(staging, destination)
        published = True
        return result
    finally:
        if not published:
            shutil.rmtree(staging, ignore_errors=True)
