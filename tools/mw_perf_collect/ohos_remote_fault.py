"""Closed authority for OHOS remote runtime-fault evidence."""
from __future__ import annotations

import ctypes
import errno
import hashlib
import json
import os
import secrets
import shutil
import stat
from pathlib import Path
from typing import Any, Mapping

from . import campaign, core, ohos_capture, ohos_registration
from .campaign import PayloadSnapshot
from .json_strict import loads as strict_json_loads
from .runtime_fault_evaluation import _producer_observation

CASE_IDS = ("callback_loss", "callback_status", "producer_identity_mismatch",
            "stage_loss", "stage_status")
RECEIPT_SCHEMA = "mw_perf.p0a.ohos_remote_fault_receipt.v1"
MATRIX_SCHEMA = "mw_perf.p0a.ohos_remote_fault_runtime_matrix.v1"
TRUSTED_ROOTS_SCHEMA = "mw_perf.p0a.ohos_remote_fault_trusted_roots.v1"
EVIDENCE_CONTRACT = "mw_perf.p0a.ohos_remote_fault_runtime_matrix.v1"
EXPECTED = {
    "stage_loss": ("stage_zero_loss", "imported trace semantic validation failed"),
    "callback_loss": ("callback_zero_loss", "imported trace semantic validation failed"),
    "producer_identity_mismatch": ("producer_identity", "imported trace semantic validation failed"),
    "stage_status": ("stage_nonzero_status", "imported trace semantic validation failed"),
    "callback_status": ("callback_nonzero_status", "imported trace semantic validation failed"),
}
_SUBJECT_FILES = frozenset({"raw/request.json", "raw/result.json", "raw/archive.tar",
                            "registration/registration.json", "registration/approval.json",
                            "validation/fault_receipt.json"})
_ANCHOR_FILES = frozenset({"registration/ohos_capture_registration.json",
                           "registration/operator_approval.json"})
_REGISTRY_FILE = "authority/ohos_remote_fault_trusted_roots.json"
_HEX_ZERO = "0" * 64
_VALIDATION_KEYS = frozenset({
    "valid", "failure_codes", "descriptor", "campaign_id", "run_id",
    "root_manifest_sha256", "evidence_manifest_sha256", "evidence_contract",
    "production_anchor", "production_build_identity_sha256",
    "build_identity_sha256", "trusted_roots_sha256", "validation_scope",
})


class OhosRemoteFaultError(ValueError):
    pass


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("ascii") + b"\n"


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _schema(value: Mapping[str, Any], name: str) -> None:
    schema = strict_json_loads(Path(__file__).with_name("schemas").joinpath(name).read_bytes())
    errors = core._schema_errors(value, schema)
    if errors:
        raise OhosRemoteFaultError(f"OHOS fault schema mismatch: {errors[0]}")


def _object(raw: bytes, label: str) -> dict[str, Any]:
    try:
        value = strict_json_loads(raw)
    except (ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise OhosRemoteFaultError(f"{label} is invalid JSON") from exc
    if not isinstance(value, dict) or raw != _canonical(value):
        raise OhosRemoteFaultError(f"{label} must be a canonical JSON object")
    return value


def _read_file_once(path: Path, label: str, limit: int) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    fd = -1
    try:
        fd = os.open(path, flags)
        before = os.fstat(fd)
        named = os.stat(path, follow_symlinks=False)
        identity = lambda item: (item.st_dev, item.st_ino, item.st_size,
                                 item.st_mtime_ns, item.st_ctime_ns)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
                or before.st_size < 1 or before.st_size > limit
                or identity(before) != identity(named)):
            raise OhosRemoteFaultError(f"{label} must be a bounded stable regular file")
        chunks: list[bytes] = []
        remaining = before.st_size + 1
        while remaining:
            chunk = os.read(fd, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        after = os.fstat(fd)
        named_after = os.stat(path, follow_symlinks=False)
        if len(raw) != before.st_size or identity(before) != identity(after) \
                or identity(after) != identity(named_after):
            raise OhosRemoteFaultError(f"{label} changed while reading")
        return raw
    except OSError as exc:
        raise OhosRemoteFaultError(f"{label} is unavailable") from exc
    finally:
        if fd >= 0:
            os.close(fd)


def _authority(registration_raw: bytes, approval_raw: bytes, *, fault: bool,
               expected_registration_sha: str,
               expected_approval_sha: str) -> tuple[dict[str, Any], dict[str, Any]]:
    if (not isinstance(expected_registration_sha, str)
            or not isinstance(expected_approval_sha, str)):
        raise OhosRemoteFaultError("external registration and approval SHA256 values are required")
    registration = _object(registration_raw, "registration")
    approval = _object(approval_raw, "operator approval")
    try:
        ohos_registration.validate_registration(
            registration, raw=registration_raw,
            expected_sha256=expected_registration_sha,
            approval=approval, approval_raw=approval_raw,
            expected_approval_sha256=expected_approval_sha)
    except (ValueError, KeyError, TypeError) as exc:
        raise OhosRemoteFaultError("OHOS registration authority is invalid") from exc
    expected_role = "fault_runtime_subject" if fault else "production_anchor"
    if registration.get("registration_role") != expected_role:
        raise OhosRemoteFaultError(f"{expected_role} registration role is required")
    return registration, approval


def _trusted_roots(registry_raw: bytes, expected_sha256: str) -> dict[str, Any]:
    if (not isinstance(expected_sha256, str) or len(expected_sha256) != 64
            or expected_sha256 == _HEX_ZERO or _digest(registry_raw) != expected_sha256):
        raise OhosRemoteFaultError("explicit external trusted-roots SHA256 mismatch")
    registry = _object(registry_raw, "OHOS remote fault trusted roots")
    _schema(registry, "ohos-remote-fault-trusted-roots.schema.v1.json")
    if [item["case_id"] for item in registry["cases"]] != list(CASE_IDS):
        raise OhosRemoteFaultError("trusted roots must contain exactly five canonical cases")
    return registry


def _case_root(registry: Mapping[str, Any], case_id: str) -> Mapping[str, str]:
    return next(item for item in registry["cases"] if item["case_id"] == case_id)


def _anchor_descriptor(registration: Mapping[str, Any], raw: bytes) -> dict[str, str]:
    return {"registration_id": registration["registration_id"], "sha256": _digest(raw),
            "build_identity_sha256": registration["build_identity"]["sha256"]}


def _archive_parts(archive_raw: bytes) -> tuple[dict[str, Any], dict[str, bytes]]:
    entries = ohos_capture._parse_ustar(archive_raw)
    manifest = _object(entries[0][1] + (b"\n" if not entries[0][1].endswith(b"\n") else b""),
                       "device capture manifest")
    return manifest, dict(entries[1:])


def _derive_receipt(request_raw: bytes, result_raw: bytes, archive_raw: bytes,
                    registration_raw: bytes, approval_raw: bytes, *,
                    expected_registration_sha: str,
                    expected_approval_sha: str) -> dict[str, Any]:
    request = _object(request_raw, "request")
    result = _object(result_raw, "result")
    registration, _approval = _authority(
        registration_raw, approval_raw, fault=True,
        expected_registration_sha=expected_registration_sha,
        expected_approval_sha=expected_approval_sha)
    case_id = request.get("fault")
    if request.get("capture_role") != "fault_runtime_subject" or case_id not in CASE_IDS:
        raise OhosRemoteFaultError("request must name exactly one canonical fault case")
    binding = {"id": registration["registration_id"], "sha256": _digest(registration_raw)}
    if request.get("trust_registration") != binding or result.get("trust_registration") != binding:
        raise OhosRemoteFaultError("capture trust registration does not equal supplied authority")
    if (request.get("campaign_id") != registration.get("campaign_id")
            or request.get("capability_id") != registration.get("capability_id")
            or request.get("runtime_images") != registration.get("runtime_images")):
        raise OhosRemoteFaultError("capture does not match fault registration")
    try:
        imported = ohos_capture.validate_capture_bytes(
            archive_raw, f"capture-{request['request_challenge']}.tar", result, request)
    except Exception as exc:
        raise OhosRemoteFaultError(str(exc)) from exc
    failure_kind, expected_error = EXPECTED[case_id]
    expected_validation = {"semantic_validation": False, "fault_error": expected_error,
                           "formal_p0a": False}
    if imported != expected_validation:
        raise OhosRemoteFaultError("fault semantic validation result is not the exact expected failure")
    failure_codes = ["P0A_ARTIFACT"]
    if imported["semantic_validation"] is False and imported["fault_error"]:
        failure_codes.append("P0A_DDS_COMM_TRACE")
    manifest, members = _archive_parts(archive_raw)
    if (manifest.get("wait_status") != 0 or manifest.get("graceful") is not True
            or manifest.get("no_escalation") is not True
            or manifest.get("descendant_detected") is not False
            or any(name == "directory_finalization.json" for name in members)):
        raise OhosRemoteFaultError("remote lifecycle or no-finalization proof is invalid")
    trace = {"raw/comm_trace/" + name: raw for name, raw in members.items()
             if name not in {"device_completion.json", "device_runtime_attestation.json"}}
    evidence = {"run_id": request["run_id"], "capability_id": request["capability_id"],
                "path_id": request["path_id"], "deployment_mode": "managed"}
    local_manifest = {"run_id": request["run_id"], "pid": result["child_pid"],
                      "pid_start_time_ticks": result["child_start_time_ticks"]}
    try:
        observation = _producer_observation(case_id, trace, local_manifest, evidence)
    except (ValueError, KeyError, TypeError) as exc:
        raise OhosRemoteFaultError("raw trace does not match declared fault case") from exc
    anchor = registration.get("production_anchor")
    if not isinstance(anchor, dict) or anchor["build_identity_sha256"] == registration["build_identity"]["sha256"]:
        raise OhosRemoteFaultError("fault and production build identities must differ")
    hashes = {item["role"]: item["sha256"] for item in registration["runtime_images"]}
    if set(hashes) != {"sut", "extension", "fastdds", "fastcdr", "cpp_runtime"} \
            or _HEX_ZERO in hashes.values() or registration["build_identity"]["sha256"] == _HEX_ZERO:
        raise OhosRemoteFaultError("five nonzero runtime image hashes and build identity are required")
    contract_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")[1]
    receipt = {
        "schema": RECEIPT_SCHEMA, "case_id": case_id, "campaign_id": request["campaign_id"],
        "subject_run_id": request["run_id"], "request_challenge": request["request_challenge"],
        "capture_nonce": manifest["nonce"], "subject_status": "invalid",
        "semantic_validation": False, "sut_exit_zero": True,
        "directory_finalization_present": False, "observed_failure_kind": failure_kind,
        "observed_error": imported["fault_error"], "observed_failure_codes": failure_codes,
        "injected_observation": observation,
        "fault_build_identity_sha256": registration["build_identity"]["sha256"],
        "production_anchor": anchor, "runtime_image_hashes": hashes,
        "contract": {"id": "mw_perf.p0a_contract.v8", "sha256": _digest(contract_raw)},
        "request_sha256": _digest(request_raw), "result_sha256": _digest(result_raw),
        "archive_sha256": _digest(archive_raw), "archive_tree_sha256": manifest["archive_tree_sha256"],
        "registration_sha256": _digest(registration_raw), "approval_sha256": _digest(approval_raw),
    }
    _schema(receipt, "ohos-remote-fault-receipt.schema.v1.json")
    return receipt


def _publication_checkpoint(_kind: str) -> None:
    """Test barrier immediately before no-replace publication."""


def _retention_checkpoint() -> None:
    """Test barrier after validation and before retaining immutable source bytes."""


def _publish_noreplace(parent_fd: int, source: str, destination: str) -> None:
    renameat2 = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
    if renameat2 is None:
        raise OhosRemoteFaultError("atomic no-replace publication is unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    if renameat2(parent_fd, os.fsencode(source), parent_fd, os.fsencode(destination), 1) != 0:
        code = ctypes.get_errno()
        if code == errno.EEXIST:
            raise FileExistsError(destination)
        raise OSError(code, os.strerror(code), destination)


def _write_tree(destination: Path, files: Mapping[str, bytes], kind: str) -> None:
    destination = Path(destination)
    if not destination.is_absolute():
        raise OhosRemoteFaultError("destination must be absolute")
    parent = destination.parent
    parent_fd = os.open(parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                        | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    temporary = f".{destination.name}-{secrets.token_hex(16)}"
    staging = parent / temporary
    try:
        os.mkdir(temporary, 0o700, dir_fd=parent_fd)
        for relative, raw in sorted(files.items()):
            target = staging / relative
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                         | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0), 0o600)
            try:
                view = memoryview(raw)
                while view:
                    written = os.write(fd, view)
                    if written <= 0:
                        raise OSError("publication write made no progress")
                    view = view[written:]
                os.fsync(fd)
            finally:
                os.close(fd)
        directories = [staging, *(item for item in staging.rglob("*") if item.is_dir())]
        for directory in reversed(directories):
            fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        _publication_checkpoint(kind)
        _publish_noreplace(parent_fd, temporary, destination.name)
        os.fsync(parent_fd)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    finally:
        os.close(parent_fd)


def materialize_ohos_remote_fault_subject(archive: str | Path, result: dict[str, Any],
                                          request: dict[str, Any], destination: str | Path, *,
                                          result_raw: bytes | None = None,
                                          request_raw: bytes | None = None,
                                          registration_path: str | Path,
                                          registration_sha256: str,
                                          approval_path: str | Path,
                                          approval_sha256: str) -> dict[str, Any]:
    if not isinstance(request_raw, bytes) or not isinstance(result_raw, bytes):
        raise OhosRemoteFaultError("immutable request and result bytes are required")
    if _object(request_raw, "request") != request or _object(result_raw, "result") != result:
        raise OhosRemoteFaultError("request/result objects do not equal supplied bytes")
    archive_raw = _read_file_once(Path(archive), "archive", ohos_capture.MAX_ARCHIVE_BYTES)
    registration_raw = _read_file_once(Path(registration_path), "registration", ohos_registration.MAX_BYTES)
    approval_raw = _read_file_once(Path(approval_path), "operator approval", ohos_registration.MAX_BYTES)
    receipt = _derive_receipt(request_raw, result_raw, archive_raw, registration_raw, approval_raw,
                              expected_registration_sha=registration_sha256,
                              expected_approval_sha=approval_sha256)
    _retention_checkpoint()
    files = {"raw/request.json": request_raw, "raw/result.json": result_raw,
             "raw/archive.tar": archive_raw, "registration/registration.json": registration_raw,
             "registration/approval.json": approval_raw,
             "validation/fault_receipt.json": _canonical(receipt)}
    _write_tree(Path(destination), files, "subject")
    return receipt


def _validate_subject_snapshot(files: Mapping[str, bytes], case_id: str,
                               root: Mapping[str, str]) -> dict[str, Any]:
    if set(files) != _SUBJECT_FILES:
        raise OhosRemoteFaultError("retained subject inventory is not exact")
    receipt = _object(files["validation/fault_receipt.json"], "fault receipt")
    derived = _derive_receipt(files["raw/request.json"], files["raw/result.json"],
                              files["raw/archive.tar"], files["registration/registration.json"],
                              files["registration/approval.json"],
                              expected_registration_sha=root["registration_sha256"],
                              expected_approval_sha=root["approval_sha256"])
    if receipt != derived or receipt.get("case_id") != case_id:
        raise OhosRemoteFaultError("fault receipt is not an exact derivation of retained bytes")
    return derived


def _validate_anchor_snapshot(files: Mapping[str, bytes],
                              root: Mapping[str, str]) -> tuple[dict[str, Any], dict[str, str]]:
    if set(files) != _ANCHOR_FILES:
        raise OhosRemoteFaultError("production anchor inventory is not exact")
    raw = files["registration/ohos_capture_registration.json"]
    registration, _ = _authority(
        raw, files["registration/operator_approval.json"], fault=False,
        expected_registration_sha=root["registration_sha256"],
        expected_approval_sha=root["approval_sha256"])
    return registration, _anchor_descriptor(registration, raw)


def _matrix_from_snapshots(subjects: Mapping[str, Mapping[str, bytes]],
                           anchor_files: Mapping[str, bytes], registry_raw: bytes,
                           expected_registry_sha256: str,
                           companion_id: str) -> dict[str, Any]:
    if tuple(sorted(subjects)) != CASE_IDS:
        raise OhosRemoteFaultError("subjects must contain exactly the five canonical cases")
    roots = _trusted_roots(registry_raw, expected_registry_sha256)
    anchor_registration, anchor = _validate_anchor_snapshot(anchor_files, roots["production"])
    cases = [_validate_subject_snapshot(subjects[case], case, _case_root(roots, case))
             for case in CASE_IDS]
    if [item["case_id"] for item in cases] != list(CASE_IDS):
        raise OhosRemoteFaultError("fault cases are not in canonical order")
    if any(item["campaign_id"] != anchor_registration["campaign_id"]
           or item["production_anchor"] != anchor for item in cases):
        raise OhosRemoteFaultError("fault subjects do not share the supplied campaign and production anchor")
    if (roots["campaign_id"] != anchor_registration["campaign_id"]
            or roots["capability_id"] != anchor_registration["capability_id"]
            or roots["threat_model"] != anchor_registration["threat_model"]):
        raise OhosRemoteFaultError("trusted roots do not match campaign, capability, or threat model")
    fault_ids = {item["fault_build_identity_sha256"] for item in cases}
    if len(fault_ids) != 1 or anchor["build_identity_sha256"] in fault_ids:
        raise OhosRemoteFaultError("fault build identity must be shared and differ from production")
    for field in ("subject_run_id", "request_challenge", "capture_nonce"):
        values = [item[field] for item in cases]
        if len(set(values)) != len(values):
            raise OhosRemoteFaultError(f"fault subject {field} values must be unique")
    case_rows = [{key: item[key] for key in (
        "case_id", "subject_run_id", "request_challenge", "capture_nonce", "archive_sha256",
        "archive_tree_sha256", "observed_failure_kind", "observed_error",
        "observed_failure_codes", "injected_observation", "runtime_image_hashes")}
        for item in cases]
    matrix = {
        "schema": MATRIX_SCHEMA, "matrix_id": "ohos-runtime-fault-matrix-" +
        _digest(_canonical(case_rows))[:24], "campaign_id": anchor_registration["campaign_id"],
        "companion_id": companion_id, "finalized": True, "conclusion": "pass",
        "production_anchor": anchor,
        "production_build_identity_sha256": anchor["build_identity_sha256"],
        "fault_build_identity_sha256": next(iter(fault_ids)), "cases": case_rows,
        "subjects_sha256": _digest(_canonical([item["archive_sha256"] for item in cases])),
        "trusted_roots_sha256": _digest(registry_raw),
        "offline_revalidation": "consistency_only",
    }
    _schema(matrix, "ohos-remote-fault-matrix.schema.v1.json")
    return matrix


def evaluate_ohos_remote_fault_matrix(subjects: dict[str, Path], destination: str | Path,
                                      production_anchor: str | Path, *,
                                      companion_id: str,
                                      trusted_roots_path: str | Path,
                                      expected_trusted_roots_sha256: str) -> dict[str, Any]:
    if not isinstance(subjects, dict) or tuple(sorted(subjects)) != CASE_IDS:
        raise OhosRemoteFaultError("subjects must contain exactly the five canonical cases")
    snapshots = {case: campaign.capture_payload_snapshot(Path(subjects[case])).files
                 for case in CASE_IDS}
    anchor = campaign.capture_payload_snapshot(Path(production_anchor)).files
    registry_raw = _read_file_once(Path(trusted_roots_path), "trusted roots",
                                   ohos_registration.MAX_BYTES)
    matrix = _matrix_from_snapshots(snapshots, anchor, registry_raw,
                                    expected_trusted_roots_sha256, companion_id)
    output: dict[str, bytes] = {"runtime_fault_matrix.json": _canonical(matrix),
                                _REGISTRY_FILE: registry_raw}
    for case in CASE_IDS:
        output.update({f"subjects/{case}/{name}": raw for name, raw in snapshots[case].items()})
    output.update({f"anchor/{name}": raw for name, raw in anchor.items()})
    _write_tree(Path(destination), output, "matrix")
    return matrix


def _matrix_snapshot(snapshot: PayloadSnapshot, trusted_roots_raw: bytes,
                     expected_trusted_roots_sha256: str) -> dict[str, Any]:
    files = snapshot.files
    expected = {"runtime_fault_matrix.json", _REGISTRY_FILE} | {f"subjects/{case}/{name}"
        for case in CASE_IDS for name in _SUBJECT_FILES} | {f"anchor/{name}" for name in _ANCHOR_FILES}
    if set(files) != expected:
        raise OhosRemoteFaultError("retained matrix inventory is not exact")
    subjects = {case: {name: files[f"subjects/{case}/{name}"] for name in _SUBJECT_FILES}
                for case in CASE_IDS}
    anchor = {name: files[f"anchor/{name}"] for name in _ANCHOR_FILES}
    supplied = _object(files["runtime_fault_matrix.json"], "runtime fault matrix")
    _schema(supplied, "ohos-remote-fault-matrix.schema.v1.json")
    if files[_REGISTRY_FILE] != trusted_roots_raw:
        raise OhosRemoteFaultError("retained trusted roots do not equal externally supplied bytes")
    derived = _matrix_from_snapshots(subjects, anchor, files[_REGISTRY_FILE],
                                     expected_trusted_roots_sha256,
                                     supplied["companion_id"])
    if supplied != derived:
        raise OhosRemoteFaultError("matrix is not an exact derivation of retained subject bytes")
    return derived


def package_ohos_remote_fault_matrix(matrix_root: str | Path, object_store: str | Path, *,
                                     campaign_id: str,
                                     trusted_roots_path: str | Path,
                                     expected_trusted_roots_sha256: str) -> dict[str, Any]:
    snapshot = campaign.capture_payload_snapshot(Path(matrix_root))
    registry_raw = _read_file_once(Path(trusted_roots_path), "trusted roots",
                                   ohos_registration.MAX_BYTES)
    report = _matrix_snapshot(snapshot, registry_raw, expected_trusted_roots_sha256)
    if report["campaign_id"] != campaign_id:
        raise OhosRemoteFaultError("package campaign does not match matrix")
    contract_raw = Path(__file__).with_name("schemas").joinpath(
        "ohos-remote-fault-matrix.schema.v1.json").read_bytes()
    descriptor = campaign.build_companion_object_from_snapshot(snapshot, Path(object_store), {
        "campaign_id": campaign_id, "role": "fault_runtime_matrix",
        "companion_id": report["companion_id"],
        "run_id": report["matrix_id"],
        "evidence_contract": {"id": EVIDENCE_CONTRACT, "sha256": _digest(contract_raw)},
        "root_manifest_sha256": _digest(snapshot.files["runtime_fault_matrix.json"]),
        "evidence_manifest_sha256": report["subjects_sha256"],
        "build_identity_sha256": report["fault_build_identity_sha256"],
    })
    object_path = Path(object_store) / "sha256" / descriptor["object_sha256"][:2] / \
        f"{descriptor['object_sha256']}.tar"
    validation = validate_ohos_remote_fault_companion(
        object_path, descriptor, trusted_roots_path=trusted_roots_path,
        expected_trusted_roots_sha256=expected_trusted_roots_sha256)
    if validation.get("valid") is not True or validation.get("descriptor") != descriptor:
        raise OhosRemoteFaultError("packaged fault matrix object failed independent validation")
    expected = _descriptor_authority(report, snapshot.files, contract_raw)
    if any(descriptor.get(key) != value for key, value in expected.items()):
        raise OhosRemoteFaultError("package descriptor semantics do not match matrix snapshot")
    return descriptor


def _descriptor_authority(report: Mapping[str, Any], payloads: Mapping[str, bytes],
                          schema_raw: bytes) -> dict[str, Any]:
    authority = {
        "campaign_id": report["campaign_id"],
        "role": "fault_runtime_matrix",
        "companion_id": report["companion_id"],
        "run_id": report["matrix_id"],
        "evidence_contract": {"id": EVIDENCE_CONTRACT, "sha256": _digest(schema_raw)},
        "root_manifest_sha256": _digest(payloads["runtime_fault_matrix.json"]),
        "evidence_manifest_sha256": report["subjects_sha256"],
        "build_identity_sha256": report["fault_build_identity_sha256"],
    }
    return authority


def _validation_result(*, valid: bool, failure_codes: list[str],
                       descriptor: dict[str, Any] | None,
                       report: Mapping[str, Any] | None = None) -> dict[str, Any]:
    """Return the closed result shape consumed by a future remote campaign."""
    result = {
        "valid": valid,
        "failure_codes": failure_codes,
        "descriptor": descriptor,
        "campaign_id": report.get("campaign_id") if report else None,
        "run_id": report.get("matrix_id") if report else None,
        "root_manifest_sha256": descriptor.get("root_manifest_sha256") if descriptor else None,
        "evidence_manifest_sha256": descriptor.get("evidence_manifest_sha256") if descriptor else None,
        "evidence_contract": descriptor.get("evidence_contract") if descriptor else None,
        "production_anchor": report.get("production_anchor") if report else None,
        "production_build_identity_sha256":
            report.get("production_build_identity_sha256") if report else None,
        "build_identity_sha256": report.get("fault_build_identity_sha256") if report else None,
        "trusted_roots_sha256": report.get("trusted_roots_sha256") if report else None,
        "validation_scope": "authoritative_external_trusted_roots" if valid else None,
    }
    if set(result) != _VALIDATION_KEYS:
        raise AssertionError("OHOS remote fault validation result shape changed")
    return result


def validate_ohos_remote_fault_companion(
        object_path: str | Path, expected_descriptor: dict[str, Any] | None = None, *,
        trusted_roots_path: str | Path,
        expected_trusted_roots_sha256: str) -> dict[str, Any]:
    """Validate packaged OHOS bytes and return a closed authority result.

    The result includes the source ``production_anchor`` registration identity
    for a future independent OHOS campaign. Generic host campaigns do not
    consume this result and retain their native contract-family binding.
    """
    if expected_descriptor is None:
        return _validation_result(
            valid=False, failure_codes=["OHOS_REMOTE_FAULT_EXPECTED_DESCRIPTOR"],
            descriptor=None)
    generic = campaign.validate_companion_object(Path(object_path), expected_descriptor)
    if generic.get("valid") is not True:
        return _validation_result(
            valid=False, failure_codes=list(generic.get("failure_codes", [])),
            descriptor=generic.get("descriptor"))
    try:
        registry_raw = _read_file_once(Path(trusted_roots_path), "trusted roots",
                                       ohos_registration.MAX_BYTES)
        payloads = campaign._read_canonical_payload_object(Path(object_path))
        report = _matrix_snapshot(PayloadSnapshot(payloads), registry_raw,
                                  expected_trusted_roots_sha256)
        schema_raw = Path(__file__).with_name("schemas").joinpath(
            "ohos-remote-fault-matrix.schema.v1.json").read_bytes()
        descriptor = generic.get("descriptor")
        if not isinstance(descriptor, dict):
            raise OhosRemoteFaultError("OHOS remote fault descriptor is unavailable")
        authority = _descriptor_authority(report, payloads, schema_raw)
        if (any(descriptor.get(key) != value for key, value in authority.items())
                or any(expected_descriptor.get(key) != value
                       for key, value in authority.items())):
            raise OhosRemoteFaultError("OHOS remote fault descriptor binding is invalid")
        return _validation_result(valid=True, failure_codes=[], descriptor=descriptor,
                                  report=report)
    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError):
        return _validation_result(
            valid=False, failure_codes=["OHOS_REMOTE_FAULT_AUTHORITY"],
            descriptor=generic.get("descriptor"))
