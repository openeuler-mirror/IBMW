"""Materialize an authenticated OHOS remote v2 capture as a remote candidate."""

from __future__ import annotations

import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import stat
from typing import Any

from . import core, dds_trace, ohos_capture, ohos_registration


class OhosP0aMaterializationError(ValueError):
    """The remote capture cannot be published as a v8 candidate run."""


_EXPECTATIONS = {
    "publish_count": 1000,
    "readers_per_sample": 1,
    "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records",
}
_CONTRACT_VERSION = "mw_perf.p0a_ohos_remote_contract.v1"
_ATTESTATION = "device_runtime_attestation.json"
_COMPLETION = "device_completion.json"


def _canonical(value: Any, *, newline: bool = True) -> bytes:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                     allow_nan=False).encode("ascii")
    return raw + (b"\n" if newline else b"")


def _canonical_absolute(value: str | Path, label: str, *, must_exist: bool) -> Path:
    raw = os.fspath(value)
    if (not isinstance(raw, str) or not os.path.isabs(raw) or raw.startswith("//")
            or "\0" in raw or os.path.normpath(raw) != raw):
        raise OhosP0aMaterializationError(f"{label} must be canonical and absolute")
    path = Path(raw)
    target = path if must_exist else path.parent
    try:
        if target.resolve(strict=True) != target:
            raise OhosP0aMaterializationError(f"{label} contains a symlink or alias")
    except OSError as error:
        raise OhosP0aMaterializationError(f"{label} parent is unavailable") from error
    return path


def _write(root: Path, relative: str, data: bytes) -> None:
    core._secure_write_new(root, relative, data)


def _remote_resolution(request: dict[str, Any], attestation: dict[str, Any],
                       completion: dict[str, Any]) -> tuple[dict[str, Any], bytes, bytes]:
    declarations = [{"role": image["role"], "name": image["name"],
        "source": image["source"], "path": image["path"], "sha256": image["sha256"]}
        for image in request["runtime_images"]]
    def observed(images: list[dict[str, Any]], phase: str) -> list[dict[str, Any]]:
        values = []
        for image in images:
            value = {"phase": phase, "role": image["role"], "name": image["name"],
                "source": image["source"], "path": image["expected_path"],
                "sha256": image["observed_sha256"],
                "device_major": image["observed_device_major"],
                "device_minor": image["observed_device_minor"],
                "inode": image["observed_inode"]}
            value["observation_id"] = core._observation_id(value)
            values.append(value)
        return values
    ready = observed(attestation["live_images"], "ready")
    completed = observed(completion["completion_images"], "completion")
    ready_by_key = {(item["role"], item["name"]): item for item in ready}
    completed_by_key = {(item["role"], item["name"]): item for item in completed}
    matches = [{**item,
        "ready_observation": ready_by_key[(item["role"], item["name"])]["observation_id"],
        "completion_observation": completed_by_key[(item["role"], item["name"])]["observation_id"]}
        for item in declarations]
    declaration_sha256 = hashlib.sha256(core._canonical_json_bytes(declarations)).hexdigest()
    proof = {"schema": "mw_perf.p0a.remote_resolution.v1",
        "run_id": request["run_id"], "capability_id": request["capability_id"],
        "path_id": request["path_id"], "deployment_mode": "managed",
        "producer": "ohos_remote_capture_v2", "finalized": True,
        "declaration_sha256": declaration_sha256, "declarations": declarations,
        "ready_observations": ready, "completion_observations": completed,
        "matches": matches, "resolved": True, "conclusion": "pass"}
    proof_bytes = _canonical(proof)
    declaration_record = {
        "schema": "mw_perf.p0a.remote_resolution_declarations.v1",
        "registration_contract_version": _CONTRACT_VERSION,
        "run_id": request["run_id"],
        "capability_id": request["capability_id"], "path_id": request["path_id"],
        "deployment_mode": "managed", "declaration_sha256": declaration_sha256,
        "declarations": declarations,
        "trust_classification": ohos_registration.TRUST_CLASSIFICATION,
    }
    return proof, proof_bytes, _canonical(declaration_record)


def _capture_members(archive_raw: bytes) -> tuple[dict[str, Any], dict[str, bytes], bytes]:
    entries = ohos_capture._parse_ustar(archive_raw)
    manifest = ohos_capture._strict_json(entries[0][1], "device_capture_manifest.json")
    return manifest, dict(entries[1:]), entries[0][1]


def _publish_noreplace(parent_fd: int, temporary_name: str, destination_name: str) -> None:
    renameat2 = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
    if renameat2 is None:
        raise OhosP0aMaterializationError("atomic no-replace directory publication is unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
                          ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    if renameat2(parent_fd, os.fsencode(temporary_name), parent_fd,
                 os.fsencode(destination_name), 1) != 0:
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            raise FileExistsError(destination_name)
        raise OSError(error, os.strerror(error), destination_name)
    os.fsync(parent_fd)


def _build_tree(staging: Path, archive_raw: bytes, result: dict[str, Any],
                request: dict[str, Any], *, result_raw: bytes,
                request_raw: bytes, registration: dict[str, Any],
                registration_raw: bytes, approval: dict[str, Any],
                approval_raw: bytes) -> dict[str, Any]:
    imported = ohos_capture.validate_capture_bytes(
        archive_raw, f'capture-{request["request_challenge"]}.tar', result, request)
    if imported.get("semantic_validation") is not True:
        raise OhosP0aMaterializationError("remote capture semantic validation did not pass")
    manifest_remote, members, manifest_raw = _capture_members(archive_raw)
    attestation_raw = members.pop(_ATTESTATION)
    completion_raw = members.pop(_COMPLETION)
    attestation = ohos_capture._strict_json(attestation_raw, _ATTESTATION)
    completion = ohos_capture._strict_json(completion_raw, _COMPLETION)
    registration_digest = hashlib.sha256(registration_raw).hexdigest()
    approval_digest = hashlib.sha256(approval_raw).hexdigest()
    binding = {"id": registration["registration_id"], "sha256": registration_digest}
    if (request.get("trust_registration") != binding
            or result.get("trust_registration") != binding
            or manifest_remote.get("trust_registration") != binding
            or request["campaign_id"] != registration["campaign_id"]
            or request["capability_id"] != registration["capability_id"]
            or request["runtime_images"] != registration["runtime_images"]
            or manifest_remote["agent_executable_sha256"] != registration["capture_agent"]["sha256"]
            or manifest_remote["boot_id"] != registration["exact_boot_id"]):
        raise OhosP0aMaterializationError("capture does not match host trust registration")

    for directory in ("environment", "topology", "capability", "raw", "normalized",
                      "logs", "registration", "validation", "raw/comm_trace"):
        (staging / directory).mkdir(mode=0o700)
    trace_root = staging / "raw/comm_trace"
    trace_fd = os.open(trace_root, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                       | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    try:
        for name, raw in members.items():
            if not name.endswith((".meta.json", ".ibct", ".ibce", ".seal.json")):
                raise OhosP0aMaterializationError("archive contains a non-trace payload member")
            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                         | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0),
                         0o600, dir_fd=trace_fd)
            try:
                view = memoryview(raw)
                while view:
                    written = os.write(fd, view)
                    if written <= 0:
                        raise OSError("short raw trace write")
                    view = view[written:]
                os.fsync(fd)
            finally:
                os.close(fd)
        producer_files = dds_trace.scan_directory_fd(trace_fd)
        if producer_files != members:
            raise OhosP0aMaterializationError("extracted raw trace set changed")
        trusted, contract_raw = core._trusted_p0a_contract(_CONTRACT_VERSION)
        identity = {"run_id": request["run_id"], "capability_id": request["capability_id"],
                    "path_id": request["path_id"], "deployment_mode": "managed"}
        identity_derivation = dds_trace.derive_identity_trace(
            producer_files, trusted["artifact_schemas"], core._schema_errors, identity,
            producer_pid=result["child_pid"],
            producer_start_time_ticks=result["child_start_time_ticks"],
            topology_generation=request["topology_generation"],
            topology_sha256=request["topology_sha256"])
        dds_trace.confirm_directory_unchanged(trace_fd, producer_files)
        dds_trace.write_directory_finalization(
            trace_fd, identity, [f"raw/comm_trace/{name}" for name in producer_files])
    finally:
        os.close(trace_fd)

    resolution, resolution_bytes, declarations_bytes = _remote_resolution(
        request, attestation, completion)
    root: dict[str, Any] = {}
    core._publish_resolution(staging, root, resolution, resolution_bytes)
    _write(staging, "registration/resolution_declarations.json", declarations_bytes)
    _write(staging, "registration/ohos_capture_registration.json", registration_raw)
    _write(staging, "registration/operator_approval.json", approval_raw)
    core._publish_identity_trace(staging, root, identity_derivation)
    contract_sha256 = hashlib.sha256(contract_raw).hexdigest()
    hook = dds_trace.derive_hook_callgraph(
        identity_derivation, resolution, resolution_bytes,
        registration_contract_sha256=contract_sha256)
    core._publish_hook_callgraph(staging, root, hook)
    normalized = dds_trace.derive_normalized_results(
        identity_derivation, resolution, resolution_bytes, hook, hook["bytes"])
    core._publish_normalized_result(staging, root, name="p0a_comm_events",
                                    relative="normalized/comm_events.jsonl",
                                    derivation=normalized["comm_events"])
    core._publish_normalized_result(staging, root, name="p0a_latency_samples",
                                    relative="normalized/latency_samples.jsonl",
                                    derivation=normalized["latency_samples"])
    latency = dds_trace.derive_communication_latency(
        identity_derivation, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        registration_contract_sha256=contract_sha256)
    core._publish_communication_latency(staging, root, latency)
    delivery = dds_trace.derive_communication_delivery(
        identity_derivation, resolution, resolution_bytes, hook, hook["bytes"], normalized,
        latency, registration_contract_sha256=contract_sha256, expectations=_EXPECTATIONS)
    core._publish_communication_delivery(staging, root, delivery)
    quality = dds_trace.derive_trace_quality(
        identity_derivation, delivery, registration_contract_version=_CONTRACT_VERSION,
        registration_contract_sha256=contract_sha256)
    core._publish_trace_quality(staging, root, quality)

    for relative, raw in (("raw/remote_capture/request.json", request_raw),
                          ("raw/remote_capture/result.json", result_raw),
                          ("raw/remote_capture/archive.tar", archive_raw)):
        _write(staging, relative, raw)
    envelope = {"schema": "mw_perf.p0a.ohos_remote_capture_envelope.v1",
        "run_id": request["run_id"], "capability_id": request["capability_id"],
        "path_id": request["path_id"], "deployment_mode": "managed",
        "registration_contract": {"version": _CONTRACT_VERSION,
            "sha256": contract_sha256},
        "trust_classification": ohos_registration.TRUST_CLASSIFICATION,
        "trust_registration": {"relative_path": "registration/ohos_capture_registration.json",
            "id": registration["registration_id"], "size": len(registration_raw),
            "sha256": registration_digest},
        "operator_approval": {"relative_path": "registration/operator_approval.json",
            "schema": approval["schema"], "size": len(approval_raw),
            "sha256": approval_digest},
        "request": {"relative_path": "raw/remote_capture/request.json",
            "size": len(request_raw), "sha256": hashlib.sha256(request_raw).hexdigest()},
        "result": {"relative_path": "raw/remote_capture/result.json",
            "size": len(result_raw), "sha256": hashlib.sha256(result_raw).hexdigest()},
        "archive": {"relative_path": "raw/remote_capture/archive.tar",
            "size": len(archive_raw), "sha256": hashlib.sha256(archive_raw).hexdigest()},
        "device_capture_manifest": {"archive_member": "device_capture_manifest.json",
            "size": len(manifest_raw), "sha256": hashlib.sha256(manifest_raw).hexdigest()},
        "runtime_attestation": {"archive_member": _ATTESTATION,
            "size": len(attestation_raw), "sha256": hashlib.sha256(attestation_raw).hexdigest()},
        "device_completion": {"archive_member": _COMPLETION,
            "size": len(completion_raw), "sha256": hashlib.sha256(completion_raw).hexdigest()},
        "finalized": True}
    _write(staging, "validation/remote_capture_envelope.json", _canonical(envelope))
    provenance = {
        "schema": "mw_perf.ohos_remote_p0a_import.v1", "formal_p0a": False,
        "run_id": request["run_id"], "campaign_id": request["campaign_id"],
        "trust_registration": binding,
        "operator_approval": {"relative_path": "registration/operator_approval.json",
            "sha256": approval_digest, "authority": "retained_consistency_only"},
        "request_challenge": request["request_challenge"], "boot_id": attestation["boot_id"],
        "producer_pid": result["child_pid"],
        "producer_start_time_ticks": result["child_start_time_ticks"],
        "transport": {
            "request_protocol": request["protocol"], "result_protocol": result["protocol"],
            "archive_sha256": result["archive_sha256"], "archive_size": result["archive_size"],
            "request_json_sha256": hashlib.sha256(request_raw).hexdigest(),
            "result_json_sha256": hashlib.sha256(result_raw).hexdigest(),
            "canonical_request_sha256": manifest_remote["request_sha256"],
            "device_manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
            "archive_tree_sha256": manifest_remote["archive_tree_sha256"],
            "capture_nonce": manifest_remote["nonce"],
            "capture_agent_executable_sha256": manifest_remote["agent_executable_sha256"],
        },
        "device_finalization": {
            "completion_sha256": hashlib.sha256(completion_raw).hexdigest(),
            "runtime_attestation_sha256": hashlib.sha256(attestation_raw).hexdigest(),
            "ready_images_sha256": attestation["ready_images_sha256"],
            "completion_images_sha256": attestation["completion_images_sha256"],
            "ready_attested": True, "completion_attested": True,
            "evidence_released": True, "completion_eof_observed": True,
        },
        "host_import": {
            "implementation": "mw_perf_collect.ohos_remote_p0a",
            "tool_version": core.VERSION, "registration_contract_sha256": contract_sha256,
            "trusted_resource_set_sha256": hashlib.sha256(
                _canonical(core._RESOURCE_SHA256, newline=False)).hexdigest(),
            "raw_source_set_sha256": identity_derivation["source_set_sha256"],
        },
        "runtime_images": {"declarations": request["runtime_images"],
                           "live": attestation["live_images"],
                           "completion": completion["completion_images"]},
    }
    provenance_raw = _canonical(provenance)
    _write(staging, "environment/ohos_remote_import.json", provenance_raw)

    trace_result = identity_derivation["trace_result"]
    trace_metadata = {
        "registration_contract_version": _CONTRACT_VERSION, "run_type": "exact_run",
        "observer": False, "capability_id": request["capability_id"],
        "path_id": request["path_id"], "deployment_mode": "managed",
        "measurement_kind": "exact_correlated",
    }
    root.update({
        "schema_version": 1, "tool_version": core.VERSION, "run_id": request["run_id"],
        "artifact_kind": "ohos_remote_p0a_candidate",
        "adapter": "generic", "mode": "managed", "run_status": "completed",
        "scenario_contract_hash": hashlib.sha256(b"{}").hexdigest(),
        "metric_availability": "unsupported", "measurement_kind": "exact_correlated",
        "remote_timing": {"schema": "mw_perf.ohos_remote_timing.v1",
            "start_time": "unavailable", "end_time": "unavailable",
            "reason": "ohos_remote_capture_v2_did_not_export_wall_clock_boundaries"},
        "pid": result["child_pid"],
        "pid_start_time_ticks": result["child_start_time_ticks"],
        "states": ["REMOTE_CAPTURE", "HOST_IMPORT", "VALIDATE", "PACKAGE"],
        "sample_count": 0, "metrics_valid": True, "adapter_readiness": True,
        "validation_profile": "p0a", "p0a_evidence_required": True,
        "run_type": "exact_run", "candidate_only": True, "formal_p0a": False,
        "valid_for_metric": False, "valid_for_comparison": False,
        "runtime_changed": True, "business_unchanged": True,
        "sut_returncode": 0,
        "dds_comm_trace": {"required": True, **core._collector_trace_summary(trace_result),
                           **trace_metadata,
                           "topology_generation": request["topology_generation"],
                           "topology_sha256": request["topology_sha256"]},
        "sample_artifacts": {"collection": "not_collected",
            "process": {"status": "not_collected"},
            "network": {"status": "not_collected"},
            "reason": "ohos_remote_capture_v2_did_not_collect_resource_samples"},
        "ohos_remote_import": {"relative_path": "environment/ohos_remote_import.json",
                               "content_sha256": hashlib.sha256(provenance_raw).hexdigest()},
    })
    p0a = core.build_p0a_evidence_manifest(
        staging, {"registration_contract_version": _CONTRACT_VERSION},
        run_id=request["run_id"], capability_id=request["capability_id"],
        path_id=request["path_id"], deployment_mode="managed", run_type="exact_run",
        observer=False, measurement_kind="exact_correlated")
    if p0a is None:
        raise OhosP0aMaterializationError("trusted remote evidence contract is unavailable")
    _write(staging, "capability/p0a_evidence_manifest.json", _canonical(p0a))
    _write(staging, "manifest.json", _canonical(root))
    validation = core.validate_dir(staging, profile="p0a")
    expected_failures = {"CANDIDATE_ONLY", "METRIC_AVAILABLE"}
    if (validation.get("valid_for_collection") is not True
            or validation.get("valid_for_metric") is not False
            or validation.get("valid_for_comparison") is not False
            or set(validation.get("failure_codes", [])) != expected_failures
            or validation.get("p0a_evidence", {}).get("valid") is not True
            or validation.get("p0a_evidence", {}).get("failure_codes") != []
            or root["dds_comm_trace"].get("status") != "exact_correlated"):
        raise OhosP0aMaterializationError("materialized candidate failed final p0a validation")
    _write(staging, "validation.json", _canonical(validation))
    return validation


def materialize_ohos_p0a_candidate(
    archive: str | Path, result: dict[str, Any], request: dict[str, Any],
    destination: str | Path, *, result_raw: bytes, request_raw: bytes,
    registration_path: str | Path, registration_sha256: str,
    approval_path: str | Path, approval_sha256: str,
) -> dict[str, Any]:
    """Validate, regenerate, and atomically publish one remote v2 candidate tree."""
    if core._resource_integrity_errors():
        raise OhosP0aMaterializationError("trusted collector resources failed integrity verification")
    if (not isinstance(result_raw, bytes) or not isinstance(request_raw, bytes)
            or ohos_capture._strict_json(result_raw, "result") != result
            or ohos_capture._strict_json(request_raw, "request") != request):
        raise OhosP0aMaterializationError("retained request/result bytes do not match JSON")
    try:
        registration_path = _canonical_absolute(
            registration_path, "registration", must_exist=True)
        approval_path = _canonical_absolute(approval_path, "operator approval", must_exist=True)
        registration, registration_raw, approval, approval_raw = \
            ohos_registration.read_registration(
                registration_path, registration_sha256, approval_path=approval_path,
                expected_approval_sha256=approval_sha256)
    except ohos_registration.OhosRegistrationError as error:
        raise OhosP0aMaterializationError(str(error)) from error
    archive_path = _canonical_absolute(archive, "archive", must_exist=True)
    output = _canonical_absolute(destination, "destination", must_exist=False)
    if output.exists() or output.is_symlink():
        raise FileExistsError(str(output))
    parent_info = os.stat(output.parent, follow_symlinks=False)
    if (not stat.S_ISDIR(parent_info.st_mode) or parent_info.st_uid != os.getuid()
            or stat.S_IMODE(parent_info.st_mode) & 0o077):
        raise OhosP0aMaterializationError("destination parent must be private and owned")
    parent_fd = os.open(output.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                        | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    opened_parent = os.fstat(parent_fd)
    if ((opened_parent.st_dev, opened_parent.st_ino) !=
            (parent_info.st_dev, parent_info.st_ino)
            or opened_parent.st_uid != os.getuid()
            or stat.S_IMODE(opened_parent.st_mode) & 0o077):
        os.close(parent_fd)
        raise OhosP0aMaterializationError("destination parent identity changed")
    temporary_name = f".mw-perf-ohos-p0a-{secrets.token_hex(16)}"
    staging = output.parent / temporary_name
    staging_fd: int | None = None
    try:
        os.mkdir(temporary_name, 0o700, dir_fd=parent_fd)
        staging_fd = os.open(temporary_name, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0), dir_fd=parent_fd)
        staging_identity = (os.fstat(staging_fd).st_dev, os.fstat(staging_fd).st_ino)
        staging_access = Path(f"/proc/self/fd/{staging_fd}")
        archive_raw = ohos_capture._read_bound_archive(archive_path, result)
        validation = _build_tree(staging_access, archive_raw, result, request,
                                 result_raw=result_raw, request_raw=request_raw,
                                 registration=registration,
                                 registration_raw=registration_raw,
                                 approval=approval, approval_raw=approval_raw)
        named = os.stat(temporary_name, dir_fd=parent_fd, follow_symlinks=False)
        if staging_identity != (named.st_dev, named.st_ino):
            raise OhosP0aMaterializationError("staging directory identity changed")
        for directory in sorted((item for item in staging_access.rglob("*") if item.is_dir()),
                                key=lambda item: len(item.parts), reverse=True):
            fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                         | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
            try: os.fsync(fd)
            finally: os.close(fd)
        os.fsync(staging_fd)
        _publish_noreplace(parent_fd, temporary_name, output.name)
        return validation
    except BaseException:
        try:
            named = os.stat(temporary_name, dir_fd=parent_fd, follow_symlinks=False)
            current = None if staging_fd is None else os.fstat(staging_fd)
            if current is not None and (named.st_dev, named.st_ino) == (current.st_dev, current.st_ino):
                shutil.rmtree(staging, ignore_errors=True)
        except FileNotFoundError:
            pass
        raise
    finally:
        if staging_fd is not None: os.close(staging_fd)
        os.close(parent_fd)
