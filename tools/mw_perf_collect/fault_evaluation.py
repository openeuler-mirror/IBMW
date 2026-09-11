"""Fail-closed evaluation of archived P0a evidence tampering."""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import shutil
import stat
import tarfile
from pathlib import Path
from typing import Any, Callable

from . import core
from .campaign import (
    PayloadSnapshot,
    build_companion_object_from_snapshot,
    capture_payload_snapshot,
    compute_archive_tree,
    compute_snapshot_archive_tree,
    validate_companion_object,
)
from .json_strict import loads as strict_json_loads


_SCHEMA = "mw_perf.p0a.fault_evidence_tamper.v1"
_THREAT_MODEL = "archived_evidence_single_byte_xor"
_EVIDENCE_MANIFEST = "capability/p0a_evidence_manifest.json"
_CONTRACT_REF = "registration/contract.json"
_TARGET_CODES = {
    "capability/hook_callgraph.json": "P0A_HOOK_CALLGRAPH",
    "capability/communication_latency.json": "P0A_COMMUNICATION_LATENCY",
    "validation/communication_delivery.json": "P0A_COMMUNICATION_DELIVERY",
    "normalized/identity_trace.jsonl": "P0A_IDENTITY_TRACE",
}
_BASELINE_FORBIDDEN_P0A_CODES = {
    "P0A_CONTRACT_HASH",
    "P0A_MANIFEST_MISSING",
    "P0A_SCHEMA",
    "P0A_ENTRY_SET",
    "P0A_ENTRY_ORDER",
    "P0A_ENTRY_GROUP",
    "P0A_STATUS",
    "P0A_TOP_LEVEL_BINDING",
}
_FAILURE_CODE = re.compile(r"[A-Z0-9_:.$\[\]-]+\Z", re.ASCII)
_REPORT_NAME = "fault_evidence_tamper.json"
_MAX_OBJECT_SIZE = 256 * 1024 * 1024
_EXPECTED_DESCRIPTOR_CODE = "FAULT_EVIDENCE_TAMPER_EXPECTED_DESCRIPTOR"
_SEMANTICS_CODE = "FAULT_EVIDENCE_TAMPER_OBJECT_SEMANTICS"
_DIRECTORY_FLAGS = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                    | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
_FILE_FLAGS = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _identity(info: os.stat_result) -> tuple[int, int, int, int, int, int]:
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _members(directory_fd: int) -> list[os.DirEntry[str]]:
    try:
        with os.scandir(directory_fd) as iterator:
            result = list(iterator)
        result.sort(key=lambda item: item.name.encode("utf-8"))
        return result
    except (OSError, UnicodeError) as exc:
        raise ValueError("anchor tree is unavailable") from exc


def _scan(directory_fd: int, prefix: str = "") -> dict[str, tuple[str, tuple[int, ...]]]:
    identities: dict[str, tuple[str, tuple[int, ...]]] = {}
    for member in _members(directory_fd):
        if member.name in (".", "..") or "/" in member.name or "\x00" in member.name:
            raise ValueError("anchor contains an invalid member name")
        relative = f"{prefix}/{member.name}" if prefix else member.name
        child_fd = -1
        try:
            entry_info = member.stat(follow_symlinks=False)
            if stat.S_ISDIR(entry_info.st_mode):
                child_fd = os.open(member.name, _DIRECTORY_FLAGS, dir_fd=directory_fd)
                opened = os.fstat(child_fd)
                if not stat.S_ISDIR(opened.st_mode) or _identity(entry_info) != _identity(opened):
                    raise ValueError("anchor directory changed during traversal")
                identities[relative] = ("directory", _identity(opened))
                identities.update(_scan(child_fd, relative))
            elif stat.S_ISREG(entry_info.st_mode):
                if entry_info.st_nlink != 1:
                    raise ValueError("anchor contains a linked file")
                child_fd = os.open(member.name, _FILE_FLAGS, dir_fd=directory_fd)
                opened = os.fstat(child_fd)
                if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1
                        or _identity(entry_info) != _identity(opened)):
                    raise ValueError("anchor file changed during traversal")
                identities[relative] = ("file", _identity(opened))
            else:
                raise ValueError("anchor contains a symlink or special file")
        except OSError as exc:
            raise ValueError("anchor member is unavailable") from exc
        finally:
            if child_fd >= 0:
                os.close(child_fd)
    return identities


def _copy_tree(source_fd: int, destination_fd: int) -> dict[str, tuple[str, tuple[int, ...]]]:
    identities: dict[str, tuple[str, tuple[int, ...]]] = {}

    def copy(directory_fd: int, output_fd: int, prefix: str) -> None:
        for member in _members(directory_fd):
            relative = f"{prefix}/{member.name}" if prefix else member.name
            source_child = destination_child = -1
            try:
                entry_info = member.stat(follow_symlinks=False)
                if stat.S_ISDIR(entry_info.st_mode):
                    source_child = os.open(member.name, _DIRECTORY_FLAGS, dir_fd=directory_fd)
                    opened = os.fstat(source_child)
                    if not stat.S_ISDIR(opened.st_mode) or _identity(entry_info) != _identity(opened):
                        raise ValueError("anchor directory changed during copy")
                    identities[relative] = ("directory", _identity(opened))
                    os.mkdir(member.name, 0o700, dir_fd=output_fd)
                    destination_child = os.open(member.name, _DIRECTORY_FLAGS, dir_fd=output_fd)
                    copy(source_child, destination_child, relative)
                    os.fsync(destination_child)
                elif stat.S_ISREG(entry_info.st_mode):
                    if entry_info.st_nlink != 1:
                        raise ValueError("anchor contains a linked file")
                    source_child = os.open(member.name, _FILE_FLAGS, dir_fd=directory_fd)
                    before = os.fstat(source_child)
                    if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
                            or _identity(entry_info) != _identity(before)):
                        raise ValueError("anchor file changed during copy")
                    identities[relative] = ("file", _identity(before))
                    destination_child = os.open(member.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                        | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0),
                        0o600, dir_fd=output_fd)
                    while True:
                        chunk = os.read(source_child, 1024 * 1024)
                        if not chunk:
                            break
                        view = memoryview(chunk)
                        while view:
                            written = os.write(destination_child, view)
                            if written <= 0:
                                raise OSError("snapshot write made no progress")
                            view = view[written:]
                    after = os.fstat(source_child)
                    if _identity(before) != _identity(after):
                        raise ValueError("anchor file changed during copy")
                    os.fsync(destination_child)
                else:
                    raise ValueError("anchor contains a symlink or special file")
            except OSError as exc:
                raise ValueError("anchor could not be copied") from exc
            finally:
                if destination_child >= 0:
                    os.close(destination_child)
                if source_child >= 0:
                    os.close(source_child)
        os.fsync(output_fd)

    copy(source_fd, destination_fd, "")
    return identities


def _read_relative(root_fd: int, relative: str) -> bytes:
    parent_fd = root_fd
    opened: list[int] = []
    try:
        parts = relative.split("/")
        for component in parts[:-1]:
            parent_fd = os.open(component, _DIRECTORY_FLAGS, dir_fd=parent_fd)
            opened.append(parent_fd)
        fd = os.open(parts[-1], _FILE_FLAGS, dir_fd=parent_fd)
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
                raise ValueError("required anchor member is not a private regular file")
            chunks: list[bytes] = []
            while chunk := os.read(fd, 1024 * 1024):
                chunks.append(chunk)
            after = os.fstat(fd)
            if _identity(before) != _identity(after):
                raise ValueError("required anchor member changed while reading")
            return b"".join(chunks)
        finally:
            os.close(fd)
    except OSError as exc:
        raise ValueError("required anchor member is unavailable") from exc
    finally:
        for fd in reversed(opened):
            os.close(fd)


def _strict_object(data: bytes, label: str) -> dict[str, Any]:
    try:
        value = strict_json_loads(data)
    except (ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is not strict JSON") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def _codes(result: Any) -> tuple[list[str], list[str]]:
    if not isinstance(result, dict):
        raise ValueError("validator result must be an object")
    top = result.get("failure_codes")
    nested_result = result.get("p0a_evidence")
    nested = nested_result.get("failure_codes") if isinstance(nested_result, dict) else None
    if (not isinstance(top, list) or not isinstance(nested, list)
            or any(not isinstance(code, str) or _FAILURE_CODE.fullmatch(code) is None
                   for code in top + nested)):
        raise ValueError("validator failure codes are incomplete")
    return sorted(set(top)), sorted(set(nested))


def _is_seal_name(relative: str) -> bool:
    basename = relative.rsplit("/", 1)[-1].lower()
    return (basename == "campaign_seal.json"
            or re.fullmatch(r"(?:.*[_-])?final[_-]seal(?:\.json)?", basename) is not None)


def _no_seal(identities: dict[str, tuple[str, tuple[int, ...]]]) -> bool:
    return not any(_is_seal_name(relative) for relative in identities)


def _baseline_metadata_bytes(root_raw: bytes, evidence_raw: bytes, contract_raw: bytes,
                             target_raw: bytes, target_relative: str, *,
                             mutated: bool = False) -> dict[str, Any]:
    root = _strict_object(root_raw, "root manifest")
    evidence = _strict_object(evidence_raw, "evidence manifest")
    contract = _strict_object(contract_raw, "registration contract")
    if (root.get("run_status") != "completed" or root.get("candidate_only") is not True
            or root.get("metric_availability") != "unsupported"
            or ("valid_for_comparison" in root
                and root["valid_for_comparison"] is not False)):
        raise ValueError("anchor manifest is not a completed unsupported candidate")
    run_id = root.get("run_id")
    version = evidence.get("registration_contract_version")
    contract_hash = _digest(contract_raw)
    try:
        _trusted_contract, trusted_raw = core._trusted_p0a_contract(version)
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("anchor registration contract is not trusted") from exc
    if (not isinstance(run_id, str) or not run_id or evidence.get("run_id") != run_id
            or evidence.get("registration_contract_ref") != _CONTRACT_REF
            or not isinstance(version, str) or not version
            or contract.get("registration_contract_version") != version
            or contract_raw != trusted_raw
            or evidence.get("registration_contract_sha256") != contract_hash):
        raise ValueError("anchor evidence contract binding is invalid")
    trace = root.get("dds_comm_trace")
    if isinstance(trace, dict) and trace.get("registration_contract_version") != version:
        raise ValueError("root manifest contract version is not bound")
    entries = evidence.get("entries")
    matches = [item for item in entries if isinstance(item, dict)
               and item.get("relative_path") == target_relative] if isinstance(entries, list) else []
    if (len(matches) != 1 or matches[0].get("status") != "required"
            or not isinstance(matches[0].get("sha256"), str)
            or not mutated and matches[0]["sha256"] != _digest(target_raw)):
        raise ValueError("target evidence entry is stale, missing, or not required")
    return {
        "run_id": run_id,
        "root_manifest_sha256": _digest(root_raw),
        "evidence_manifest_sha256": _digest(evidence_raw),
        "registration_contract": {"id": version, "sha256": contract_hash},
        "target": target_raw,
        "target_manifest_sha256": matches[0]["sha256"],
    }


def _baseline_metadata(root_fd: int, target_relative: str, *, mutated: bool = False) -> dict[str, Any]:
    return _baseline_metadata_bytes(
        _read_relative(root_fd, "manifest.json"),
        _read_relative(root_fd, _EVIDENCE_MANIFEST),
        _read_relative(root_fd, _CONTRACT_REF),
        _read_relative(root_fd, target_relative),
        target_relative,
        mutated=mutated,
    )


def _mutate(snapshot_fd: int, relative: str, offset: int) -> tuple[int, str, str]:
    if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0:
        raise ValueError("mutation_offset must be a nonnegative integer")
    parent_fd = snapshot_fd
    opened: list[int] = []
    try:
        parts = relative.split("/")
        for component in parts[:-1]:
            parent_fd = os.open(component, _DIRECTORY_FLAGS, dir_fd=parent_fd)
            opened.append(parent_fd)
        fd = os.open(parts[-1], os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
                     | getattr(os, "O_CLOEXEC", 0), dir_fd=parent_fd)
        try:
            before_info = os.fstat(fd)
            if not stat.S_ISREG(before_info.st_mode) or before_info.st_nlink != 1:
                raise ValueError("snapshot target is not a private regular file")
            size = before_info.st_size
            if offset >= size:
                raise ValueError("mutation_offset is outside the target")
            os.lseek(fd, 0, os.SEEK_SET)
            before_data = b""
            while chunk := os.read(fd, 1024 * 1024):
                before_data += chunk
            changed = bytes([before_data[offset] ^ 0x01])
            if os.pwrite(fd, changed, offset) != 1:
                raise OSError("mutation write was incomplete")
            os.fsync(fd)
            after_info = os.fstat(fd)
            os.lseek(fd, 0, os.SEEK_SET)
            after_data = b""
            while chunk := os.read(fd, 1024 * 1024):
                after_data += chunk
            if (after_info.st_size != size or len(after_data) != size
                    or after_data[:offset] != before_data[:offset]
                    or after_data[offset + 1:] != before_data[offset + 1:]
                    or after_data[offset] != before_data[offset] ^ 0x01):
                raise ValueError("snapshot mutation was not exactly one byte")
            before_hash, after_hash = _digest(before_data), _digest(after_data)
            if before_hash == after_hash:
                raise ValueError("snapshot mutation did not change the target digest")
            return size, before_hash, after_hash
        finally:
            os.close(fd)
    except OSError as exc:
        raise ValueError("snapshot target could not be mutated") from exc
    finally:
        for fd in reversed(opened):
            os.close(fd)


def _validate_result(result: Any, *, baseline: bool) -> tuple[list[str], list[str]]:
    if not isinstance(result, dict):
        raise ValueError("validator result must be an object")
    if (result.get("valid_for_collection") is not True
            or result.get("valid_for_metric") is not False
            or result.get("valid_for_comparison") is not False):
        raise ValueError("validator did not preserve candidate-only semantics")
    top, nested = _codes(result)
    if baseline and "P0A_ARTIFACT" not in nested:
        raise ValueError("baseline lacks nested P0A_ARTIFACT")
    if baseline and _BASELINE_FORBIDDEN_P0A_CODES.intersection(nested):
        raise ValueError("baseline has a forbidden nested P0a structural failure")
    return top, nested


def _source_state(source_fd: int, anchor: Path) -> dict[str, tuple[str, tuple[int, ...]]]:
    path_info = os.stat(anchor, follow_symlinks=False)
    opened = os.fstat(source_fd)
    if not stat.S_ISDIR(path_info.st_mode) or _identity(path_info) != _identity(opened):
        raise ValueError("anchor changed during evaluation")
    return {"": ("directory", _identity(opened)), **_scan(source_fd)}


def _cleanup_failed_evaluation(experiment: Path, original: BaseException) -> None:
    try:
        (experiment / "payload" / _REPORT_NAME).unlink(missing_ok=True)
        shutil.rmtree(experiment)
    except Exception:
        raise RuntimeError("evidence tamper evaluation cleanup failed") from original


def _read_immutable_object(path: Path) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size > _MAX_OBJECT_SIZE:
            raise ValueError("fault companion is not a bounded regular file")
        chunks: list[bytes] = []
        remaining = _MAX_OBJECT_SIZE + 1
        while remaining:
            chunk = os.read(fd, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        after = os.fstat(fd)
        linked = os.stat(path, follow_symlinks=False)
        identity = lambda item: (item.st_dev, item.st_ino, item.st_mode, item.st_size,
                                 item.st_mtime_ns, item.st_ctime_ns)
        if (len(raw) > _MAX_OBJECT_SIZE or identity(before) != identity(after)
                or identity(after) != identity(linked)):
            raise ValueError("fault companion changed while being read")
        return raw
    finally:
        os.close(fd)


def validate_evidence_tamper_companion(
    object_path: Path,
    expected_descriptor: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Validate archived evidence-tamper semantics and descriptor bindings."""
    generic = validate_companion_object(Path(object_path), expected_descriptor)
    if expected_descriptor is None:
        return {"valid": False, "failure_codes": [_EXPECTED_DESCRIPTOR_CODE],
                "descriptor": generic.get("descriptor")}
    codes = set(generic.get("failure_codes", []))
    if codes:
        return {"valid": False, "failure_codes": sorted(codes),
                "descriptor": generic.get("descriptor")}

    anchors: dict[str, Any] = {}
    try:
        raw = _read_immutable_object(Path(object_path))
        if _digest(raw) != expected_descriptor.get("object_sha256"):
            raise ValueError("fault companion object hash differs from descriptor")
        payloads: dict[str, bytes] = {}
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
            for member in archive:
                if member.isreg() and member.name.startswith("payload/"):
                    stream = archive.extractfile(member)
                    if stream is None:
                        raise ValueError("fault companion payload is unreadable")
                    payloads[member.name.removeprefix("payload/")] = stream.read()

        if (_REPORT_NAME not in payloads
                or not any(name.startswith("anchor_snapshot/") for name in payloads)
                or any(name != _REPORT_NAME and not name.startswith("anchor_snapshot/")
                       for name in payloads)
                or any(_is_seal_name(name) for name in payloads)):
            raise ValueError("fault companion payload set is invalid")
        report_raw = payloads[_REPORT_NAME]
        report = _strict_object(report_raw, "fault report")
        schema = _strict_object((Path(__file__).parent / "schemas" /
            "fault-evidence-tamper-result.schema.v1.json").read_bytes(), "result schema")
        if (core._schema_errors(report, schema)
                or report_raw != core._canonical_json_bytes(report, newline=True)):
            raise ValueError("fault report schema or canonical encoding is invalid")
        classification = report.get("campaign_classification")
        if (classification != {"classification": "candidate",
                                "metric_availability": "unsupported",
                                "valid_for_comparison": False,
                                "failure_code": "P0A_ARTIFACT"}
                or report.get("schema") != _SCHEMA
                or report.get("threat_model") != _THREAT_MODEL
                or report.get("finalized") is not True
                or report.get("conclusion") != "pass"
                or report.get("subject_status") != "invalid"):
            raise ValueError("fault report classification is invalid")

        mutation = report["mutation"]
        target_relative = mutation["target_relative"]
        expected_code = _TARGET_CODES.get(target_relative)
        prefix = "anchor_snapshot/"
        required = {
            "root": prefix + "manifest.json",
            "evidence": prefix + _EVIDENCE_MANIFEST,
            "contract": prefix + _CONTRACT_REF,
            "target": prefix + target_relative,
        }
        if any(name not in payloads for name in required.values()):
            raise ValueError("fault companion anchor metadata is incomplete")
        for key in ("root", "evidence"):
            _strict_object(payloads[required[key]], f"anchor {key}")
        target_raw = payloads[required["target"]]
        bound = _baseline_metadata_bytes(
            payloads[required["root"]], payloads[required["evidence"]],
            payloads[required["contract"]], target_raw, target_relative, mutated=True)
        offset = mutation.get("offset")
        if (expected_code is None or not isinstance(offset, int) or isinstance(offset, bool)
                or offset < 0 or offset >= len(target_raw) or mutation.get("xor_mask") != 1):
            raise ValueError("fault mutation is not a supported one-byte XOR")
        original = bytearray(target_raw)
        original[offset] ^= 1
        original_raw = bytes(original)
        if (mutation.get("size") != len(target_raw)
                or mutation.get("after_sha256") != _digest(target_raw)
                or mutation.get("before_sha256") != _digest(original_raw)
                or bound["target_manifest_sha256"] != _digest(original_raw)):
            raise ValueError("fault mutation byte binding is invalid")

        snapshot_files = {name.removeprefix(prefix): data for name, data in payloads.items()
                          if name.startswith(prefix)}
        current_tree = compute_snapshot_archive_tree(PayloadSnapshot(snapshot_files))["sha256"]
        original_files = dict(snapshot_files)
        original_files[target_relative] = original_raw
        original_tree = compute_snapshot_archive_tree(PayloadSnapshot(original_files))["sha256"]
        anchor = report["anchor"]
        if (current_tree != report.get("archive_tree_after_sha256")
                or original_tree != anchor.get("archive_tree_before_sha256")
                or bound["run_id"] != anchor.get("run_id")
                or bound["root_manifest_sha256"] != anchor.get("root_manifest_sha256")
                or bound["evidence_manifest_sha256"] != anchor.get("evidence_manifest_sha256")
                or bound["registration_contract"] != anchor.get("registration_contract")):
            raise ValueError("fault archive anchor binding is invalid")

        validation = report["validation"]
        baseline_top = validation["baseline_top_codes"]
        baseline_p0a = validation["baseline_p0a_codes"]
        observed_top = validation["observed_top_codes"]
        observed_p0a = validation["observed_p0a_codes"]
        for values in (baseline_top, baseline_p0a, observed_top, observed_p0a):
            if values != sorted(set(values)):
                raise ValueError("fault validation codes are not canonical sets")
        if ("P0A_ARTIFACT" not in baseline_p0a
                or _BASELINE_FORBIDDEN_P0A_CODES.intersection(baseline_p0a)
                or not set(baseline_top).issubset(observed_top)
                or not set(baseline_p0a).issubset(observed_p0a)
                or expected_code in baseline_p0a or expected_code not in observed_p0a
                or validation.get("expected_new_p0a_code") != expected_code):
            raise ValueError("fault validation transition is invalid")

        descriptor = generic.get("descriptor")
        if (not isinstance(descriptor, dict)
                or descriptor.get("role") != "fault_evidence_tamper"
                or descriptor.get("run_id") != bound["run_id"]
                or descriptor.get("root_manifest_sha256") != bound["root_manifest_sha256"]
                or descriptor.get("evidence_manifest_sha256") != bound["evidence_manifest_sha256"]
                or descriptor.get("evidence_contract") != bound["registration_contract"]):
            raise ValueError("fault descriptor anchor binding is invalid")
        anchors = {
            "run_id": bound["run_id"],
            "root_manifest_sha256": bound["root_manifest_sha256"],
            "evidence_manifest_sha256": bound["evidence_manifest_sha256"],
            "build_identity_sha256": descriptor["build_identity_sha256"],
            "evidence_contract": bound["registration_contract"],
        }
        anchors["anchor"] = dict(anchors)
    except (OSError, tarfile.TarError, UnicodeError, json.JSONDecodeError, KeyError,
            TypeError, ValueError):
        codes.add(_SEMANTICS_CODE)
    return {"valid": not codes, "failure_codes": sorted(codes),
            "descriptor": generic.get("descriptor"), **anchors}


def evaluate_evidence_tamper(
    anchor_run: Path,
    experiment_root: Path,
    target_relative: str,
    mutation_offset: int = 0,
    *,
    validator: Callable[..., dict[str, Any]] = core.validate_dir,
) -> dict[str, Any]:
    """Copy one candidate anchor, mutate one allowlisted byte, and prove detection."""
    expected_code = _TARGET_CODES.get(target_relative)
    if expected_code is None:
        raise ValueError("target_relative is not allowlisted")
    anchor = Path(anchor_run)
    experiment = Path(experiment_root)
    created = False
    source_fd = payload_fd = snapshot_fd = -1
    try:
        try:
            os.mkdir(experiment, 0o700)
            created = True
        except FileExistsError as exc:
            raise ValueError("experiment_root must not already exist") from exc
        payload = experiment / "payload"
        snapshot = payload / "anchor_snapshot"
        os.mkdir(payload, 0o700)
        os.mkdir(snapshot, 0o700)
        source_fd = os.open(anchor, _DIRECTORY_FLAGS)
        source_root = os.fstat(source_fd)
        source_path = os.stat(anchor, follow_symlinks=False)
        if not stat.S_ISDIR(source_path.st_mode) or _identity(source_root) != _identity(source_path):
            raise ValueError("anchor_run must be a stable real directory")
        initial = {"": ("directory", _identity(source_root)), **_scan(source_fd)}
        if not _no_seal(initial):
            raise ValueError("final campaign seal already exists")
        snapshot_fd = os.open(snapshot, _DIRECTORY_FLAGS)
        copied = {"": ("directory", _identity(source_root)), **_copy_tree(source_fd, snapshot_fd)}
        if copied != initial:
            raise ValueError("anchor changed while snapshot was copied")
        second = {"": ("directory", _identity(os.fstat(source_fd))), **_scan(source_fd)}
        if second != initial:
            raise ValueError("anchor changed after snapshot copy")
        archive_before = compute_archive_tree(snapshot)["sha256"]
        metadata = _baseline_metadata(snapshot_fd, target_relative)
        if _source_state(source_fd, anchor) != initial:
            raise ValueError("anchor changed before copied-baseline validation")
        baseline = validator(snapshot, profile="p0a")
        baseline_top, baseline_p0a = _validate_result(baseline, baseline=True)
        if _source_state(source_fd, anchor) != initial:
            raise ValueError("anchor changed during copied-baseline validation")
        size, before_hash, after_hash = _mutate(snapshot_fd, target_relative, mutation_offset)
        if before_hash != _digest(metadata["target"]):
            raise ValueError("snapshot target does not match the bound anchor target")
        observed = validator(snapshot, profile="p0a")
        observed_top, observed_p0a = _validate_result(observed, baseline=False)
        if (not set(baseline_top).issubset(observed_top)
                or not set(baseline_p0a).issubset(observed_p0a)
                or expected_code in baseline_p0a or expected_code not in observed_p0a):
            raise ValueError("tamper validator did not add the target-specific nested P0a code")
        after_scan = {"": ("directory", _identity(os.fstat(snapshot_fd))), **_scan(snapshot_fd)}
        if not _no_seal(after_scan):
            raise ValueError("mutated snapshot contains a final campaign seal")
        archive_after = compute_archive_tree(snapshot)["sha256"]
        if archive_before == archive_after:
            raise ValueError("archive tree digest did not change")
        final_source_path = os.stat(anchor, follow_symlinks=False)
        final_source = {"": ("directory", _identity(os.fstat(source_fd))), **_scan(source_fd)}
        if (_identity(final_source_path) != _identity(source_root) or final_source != initial):
            raise ValueError("anchor changed during evaluation")
        report = {
            "schema": _SCHEMA,
            "campaign_classification": {
                "classification": "candidate",
                "metric_availability": "unsupported",
                "valid_for_comparison": False,
                "failure_code": "P0A_ARTIFACT",
            },
            "threat_model": _THREAT_MODEL,
            "finalized": True,
            "conclusion": "pass",
            "subject_status": "invalid",
            "anchor": {
                "run_id": metadata["run_id"],
                "root_manifest_sha256": metadata["root_manifest_sha256"],
                "evidence_manifest_sha256": metadata["evidence_manifest_sha256"],
                "registration_contract": metadata["registration_contract"],
                "archive_tree_before_sha256": archive_before,
            },
            "mutation": {
                "target_relative": target_relative,
                "offset": mutation_offset,
                "size": size,
                "before_sha256": before_hash,
                "after_sha256": after_hash,
                "xor_mask": 1,
            },
            "validation": {
                "baseline_top_codes": baseline_top,
                "baseline_p0a_codes": baseline_p0a,
                "observed_top_codes": observed_top,
                "observed_p0a_codes": observed_p0a,
                "expected_new_p0a_code": expected_code,
            },
            "archive_tree_after_sha256": archive_after,
        }
        schema = _strict_object((Path(__file__).parent / "schemas" /
            "fault-evidence-tamper-result.schema.v1.json").read_bytes(), "result schema")
        errors = core._schema_errors(report, schema)
        if errors:
            raise RuntimeError("fault evaluator produced an invalid report: " + ",".join(errors))
        report_path = payload / _REPORT_NAME
        fd = os.open(report_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                     | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0), 0o600)
        try:
            data = core._canonical_json_bytes(report, newline=True)
            view = memoryview(data)
            while view:
                view = view[os.write(fd, view):]
            os.fsync(fd)
        finally:
            os.close(fd)
        payload_fd = os.open(payload, _DIRECTORY_FLAGS)
        published_members = _scan(payload_fd)
        if (published_members.get(_REPORT_NAME, (None,))[0] != "file"
                or published_members.get("anchor_snapshot", (None,))[0] != "directory"
                or any(relative not in {_REPORT_NAME, "anchor_snapshot"}
                       and not relative.startswith("anchor_snapshot/")
                       for relative in published_members)
                or not _no_seal(published_members)):
            raise ValueError("published experiment has an unexpected member or final seal")
        os.fsync(payload_fd)
        os.fsync(snapshot_fd)
        final_source_path = os.stat(anchor, follow_symlinks=False)
        final_source = {"": ("directory", _identity(os.fstat(source_fd))), **_scan(source_fd)}
        if (_identity(final_source_path) != _identity(source_root) or final_source != initial):
            raise ValueError("anchor changed before report publication completed")
        return report
    except BaseException as exc:
        if created:
            _cleanup_failed_evaluation(experiment, exc)
        if isinstance(exc, ValueError):
            raise
        if isinstance(exc, Exception):
            raise ValueError("evidence tamper evaluation failed") from exc
        raise
    finally:
        for fd in (payload_fd, snapshot_fd, source_fd):
            if fd >= 0:
                os.close(fd)


def build_evidence_tamper_companion(
    report: dict[str, Any],
    experiment_root: Path,
    *,
    campaign_id: str,
    companion_id: str,
    build_identity_sha256: str,
    object_store: Path,
) -> dict[str, Any]:
    """Package a completed tamper experiment using report-bound metadata."""
    payload = Path(experiment_root) / "payload"
    snapshot = capture_payload_snapshot(payload)
    files = snapshot.files
    if (_REPORT_NAME not in files
            or not any(relative.startswith("anchor_snapshot/") for relative in files)
            or any(relative != _REPORT_NAME and not relative.startswith("anchor_snapshot/")
                   for relative in files)
            or any(_is_seal_name(relative) for relative in files)):
        raise ValueError("experiment payload has an unexpected member or final seal")
    report_raw = files[_REPORT_NAME]
    stored = _strict_object(report_raw, "fault report")
    schema = _strict_object((Path(__file__).parent / "schemas" /
        "fault-evidence-tamper-result.schema.v1.json").read_bytes(), "result schema")
    if (stored != report or core._schema_errors(stored, schema)
            or report_raw != core._canonical_json_bytes(stored, newline=True)
            or stored.get("schema") != _SCHEMA or stored.get("conclusion") != "pass"):
        raise ValueError("report is not the completed experiment report")
    anchor = stored.get("anchor")
    if not isinstance(anchor, dict):
        raise ValueError("report anchor metadata is missing")
    target_relative = stored["mutation"]["target_relative"]
    prefix = "anchor_snapshot/"
    required = {
        "root": prefix + "manifest.json",
        "evidence": prefix + _EVIDENCE_MANIFEST,
        "contract": prefix + _CONTRACT_REF,
        "target": prefix + target_relative,
    }
    if any(relative not in files for relative in required.values()):
        raise ValueError("mutated snapshot metadata is incomplete")
    target_raw = files[required["target"]]
    bound = _baseline_metadata_bytes(
        files[required["root"]], files[required["evidence"]],
        files[required["contract"]], target_raw, target_relative, mutated=True)
    mutation = stored["mutation"]
    offset = mutation.get("offset")
    if (not isinstance(offset, int) or isinstance(offset, bool) or offset < 0
            or offset >= len(target_raw) or mutation.get("xor_mask") != 1):
        raise ValueError("mutated snapshot does not describe a one-byte XOR")
    original = bytearray(target_raw)
    original[offset] ^= 1
    original_sha256 = _digest(bytes(original))
    if (bound["run_id"] != anchor.get("run_id")
            or bound["root_manifest_sha256"] != anchor.get("root_manifest_sha256")
            or bound["evidence_manifest_sha256"] != anchor.get("evidence_manifest_sha256")
            or bound["registration_contract"] != anchor.get("registration_contract")
            or original_sha256 != mutation.get("before_sha256")
            or original_sha256 != bound["target_manifest_sha256"]
            or len(target_raw) != mutation.get("size")
            or _digest(target_raw) != mutation.get("after_sha256")
            or compute_snapshot_archive_tree(snapshot, "anchor_snapshot")["sha256"]
               != stored.get("archive_tree_after_sha256")):
        raise ValueError("mutated snapshot does not match the completed report")
    metadata = {
        "campaign_id": campaign_id,
        "role": "fault_evidence_tamper",
        "companion_id": companion_id,
        "run_id": bound["run_id"],
        "evidence_contract": bound["registration_contract"],
        "root_manifest_sha256": bound["root_manifest_sha256"],
        "evidence_manifest_sha256": bound["evidence_manifest_sha256"],
        "build_identity_sha256": build_identity_sha256,
    }
    descriptor = build_companion_object_from_snapshot(snapshot, Path(object_store), metadata)
    object_sha256 = descriptor["object_sha256"]
    object_path = (Path(object_store) / "sha256" / object_sha256[:2]
                   / f"{object_sha256}.tar")
    if not validate_evidence_tamper_companion(object_path, descriptor)["valid"]:
        raise ValueError("built companion object failed evidence-tamper validation")
    return descriptor
