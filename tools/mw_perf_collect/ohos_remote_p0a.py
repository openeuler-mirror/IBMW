"""Materialize an authenticated OHOS remote v2 capture as a v8 candidate run."""

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

from . import core, dds_trace, ohos_capture


class OhosP0aMaterializationError(ValueError):
    """The remote capture cannot be published as a v8 candidate run."""


_EXPECTATIONS = {
    "publish_count": 1000,
    "readers_per_sample": 1,
    "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records",
}
_CONTRACT_VERSION = "mw_perf.p0a_contract.v8"
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


def _remote_resolution(request: dict[str, Any], attestation: dict[str, Any]) -> tuple[dict[str, Any], bytes, bytes]:
    live_by_key = {(item["role"], item["name"]): item for item in attestation["live_images"]}
    declarations = []
    observations = []
    for image in request["runtime_images"]:
        observed = live_by_key[(image["role"], image["name"])]
        device = os.makedev(observed["observed_device_major"],
                           observed["observed_device_minor"])
        declaration = {
            "role": image["role"], "name": image["name"], "source": image["source"],
            "expected_path": image["path"], "sha256": image["sha256"],
            "expected_device": device, "expected_inode": observed["observed_inode"],
            # The v2 wire attestation deliberately carries content and inode identity,
            # not a receiver-reopenable device file size.
            "expected_size": 0,
        }
        observation = {
            "run_id": request["run_id"], "pid": attestation["child_pid"],
            "start_time_ticks": attestation["child_start_time_ticks"],
            "source": image["source"], "resolved_path": image["path"],
            "device": device, "inode": observed["observed_inode"], "size": 0,
            "sha256": observed["observed_sha256"],
            "identity_source": "proc_exe" if image["source"] == "exe" else "proc_map_file",
        }
        if image["source"] == "maps":
            observation["mapping"] = {
                "start_address": "0", "end_address": "1", "permissions": "r-xp",
                "device": (f'{observed["observed_device_major"]:x}:'
                           f'{observed["observed_device_minor"]:x}'),
                "inode": observed["observed_inode"], "deleted": False,
            }
        observation["observation_id"] = core._observation_id(observation)
        declarations.append(declaration)
        observations.append(observation)
    declarations.sort(key=lambda item: tuple(item[field].encode("utf-8") for field in (
        "role", "name", "source", "expected_path", "sha256")))
    observations.sort(key=lambda item: item["observation_id"])
    proof, proof_bytes = core._build_resolution_proof({"run_id": request["run_id"]}, {
        "capability_id": request["capability_id"], "path_id": request["path_id"]},
        declarations, observations)
    declaration_record = {
        "schema": "mw_perf.p0a.resolution_declarations.v1",
        "registration_contract_version": _CONTRACT_VERSION,
        "run_id": request["run_id"],
        "declaration_sha256": proof["declaration_sha256"],
        "declarations": declarations,
    }
    return proof, proof_bytes, _canonical(declaration_record)


def _capture_members(archive: Path, result: dict[str, Any]) -> tuple[dict[str, Any], dict[str, bytes], bytes]:
    archive_raw = ohos_capture._read_bound_archive(archive, result)
    entries = ohos_capture._parse_ustar(archive_raw)
    manifest = ohos_capture._strict_json(entries[0][1], "device_capture_manifest.json")
    return manifest, dict(entries[1:]), entries[0][1]


def _sample_files(request: dict[str, Any], result: dict[str, Any]) -> tuple[bytes, bytes]:
    process = {"timestamp": 0, "processes": [{
        "pid": result["child_pid"], "start_time_ticks": result["child_start_time_ticks"],
        "cpu_seconds": 0, "rss_bytes": 0, "pss_bytes": None, "threads": 1,
    }]}
    network = {"timestamp": 0, "interfaces": {}}
    return _canonical(process), _canonical(network)


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


def _build_tree(staging: Path, archive: Path, result: dict[str, Any], request: dict[str, Any],
                *, result_sha256: str, request_sha256: str) -> dict[str, Any]:
    imported = ohos_capture.validate_capture_semantics(archive, result, request)
    if imported.get("semantic_validation") is not True:
        raise OhosP0aMaterializationError("remote capture semantic validation did not pass")
    manifest_remote, members, manifest_raw = _capture_members(archive, result)
    attestation_raw = members.pop(_ATTESTATION)
    completion_raw = members.pop(_COMPLETION)
    attestation = ohos_capture._strict_json(attestation_raw, _ATTESTATION)
    completion = ohos_capture._strict_json(completion_raw, _COMPLETION)

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

    resolution, resolution_bytes, declarations_bytes = _remote_resolution(request, attestation)
    root: dict[str, Any] = {}
    core._publish_resolution(staging, root, resolution, resolution_bytes)
    _write(staging, "registration/resolution_declarations.json", declarations_bytes)
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

    process_raw, network_raw = _sample_files(request, result)
    _write(staging, "raw/process.jsonl", process_raw)
    _write(staging, "raw/network.jsonl", network_raw)
    provenance = {
        "schema": "mw_perf.ohos_remote_p0a_import.v1", "formal_p0a": False,
        "run_id": request["run_id"], "campaign_id": request["campaign_id"],
        "request_challenge": request["request_challenge"], "boot_id": attestation["boot_id"],
        "producer_pid": result["child_pid"],
        "producer_start_time_ticks": result["child_start_time_ticks"],
        "transport": {
            "request_protocol": request["protocol"], "result_protocol": result["protocol"],
            "archive_sha256": result["archive_sha256"], "archive_size": result["archive_size"],
            "request_json_sha256": request_sha256, "result_json_sha256": result_sha256,
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
        "adapter": "generic", "mode": "managed", "run_status": "completed",
        "scenario_contract_hash": hashlib.sha256(b"{}").hexdigest(),
        "metric_availability": "unsupported", "measurement_kind": "exact_correlated",
        "start_time": 0, "end_time": 0, "pid": result["child_pid"],
        "pid_start_time_ticks": result["child_start_time_ticks"],
        "states": ["REMOTE_CAPTURE", "HOST_IMPORT", "VALIDATE", "PACKAGE"],
        "sample_count": 1, "metrics_valid": True, "adapter_readiness": True,
        "validation_profile": "p0a", "p0a_evidence_required": True,
        "run_type": "exact_run", "candidate_only": True, "formal_p0a": False,
        "runtime_changed": True, "business_unchanged": True,
        "sut_returncode": 0,
        "dds_comm_trace": {"required": True, **core._collector_trace_summary(trace_result),
                           **trace_metadata,
                           "topology_generation": request["topology_generation"],
                           "topology_sha256": request["topology_sha256"]},
        "sample_artifacts": core._sample_artifact_evidence(staging),
        "ohos_remote_import": {"relative_path": "environment/ohos_remote_import.json",
                               "content_sha256": hashlib.sha256(provenance_raw).hexdigest()},
    })
    p0a = core.build_p0a_evidence_manifest(
        staging, {"registration_contract_version": _CONTRACT_VERSION},
        run_id=request["run_id"], capability_id=request["capability_id"],
        path_id=request["path_id"], deployment_mode="managed", run_type="exact_run",
        observer=False, measurement_kind="exact_correlated")
    if p0a is None:
        raise OhosP0aMaterializationError("trusted v8 evidence contract is unavailable")
    core.write_json(staging / "capability/p0a_evidence_manifest.json", p0a)
    core.write_json(staging / "manifest.json", root)
    validation = core.validate_dir(staging, profile="p0a")
    expected_failures = {"CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"}
    if (validation.get("valid_for_collection") is not True
            or validation.get("valid_for_metric") is not False
            or validation.get("valid_for_comparison") is not False
            or set(validation.get("failure_codes", [])) != expected_failures
            or validation.get("p0a_evidence", {}).get("failure_codes") != ["P0A_ARTIFACT"]
            or root["dds_comm_trace"].get("status") != "exact_correlated"):
        raise OhosP0aMaterializationError("materialized candidate failed final p0a validation")
    core.write_json(staging / "validation.json", validation)
    return validation


def materialize_ohos_p0a_candidate(
    archive: str | Path, result: dict[str, Any], request: dict[str, Any],
    destination: str | Path, *, result_sha256: str, request_sha256: str,
) -> dict[str, Any]:
    """Validate, regenerate, and atomically publish one remote v2 candidate tree."""
    if core._resource_integrity_errors():
        raise OhosP0aMaterializationError("trusted collector resources failed integrity verification")
    for label, digest in (("result", result_sha256), ("request", request_sha256)):
        if (not isinstance(digest, str) or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)):
            raise OhosP0aMaterializationError(f"{label} JSON SHA-256 is malformed")
    archive_path = _canonical_absolute(archive, "archive", must_exist=True)
    output = _canonical_absolute(destination, "destination", must_exist=False)
    if output.exists() or output.is_symlink():
        raise FileExistsError(str(output))
    parent_info = os.stat(output.parent, follow_symlinks=False)
    if not stat.S_ISDIR(parent_info.st_mode):
        raise OhosP0aMaterializationError("destination parent is not a directory")
    parent_fd = os.open(output.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                        | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    temporary_name = f".mw-perf-ohos-p0a-{secrets.token_hex(16)}"
    staging = output.parent / temporary_name
    try:
        os.mkdir(temporary_name, 0o700, dir_fd=parent_fd)
        os.chmod(staging, 0o700, follow_symlinks=False)
        validation = _build_tree(staging, archive_path, result, request,
                                 result_sha256=result_sha256,
                                 request_sha256=request_sha256)
        _publish_noreplace(parent_fd, temporary_name, output.name)
        return validation
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    finally:
        os.close(parent_fd)
