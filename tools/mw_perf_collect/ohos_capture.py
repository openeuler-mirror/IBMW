"""Fail-closed host import validation for native OHOS capture archives."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
from typing import Any

MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_MEMBER_BYTES = 32 * 1024 * 1024
MAX_MEMBERS = 4097
MAX_RECORDS = 100_000
_BLOCK = 512
_MANIFEST = "device_capture_manifest.json"
_HEX64 = re.compile(r"[0-9a-f]{64}")
_RESULT_KEYS = {
    "protocol", "snapshot_valid", "semantic_validation", "archive_path", "request_challenge",
    "archive_sha256", "archive_size", "child_pid", "child_start_time_ticks", "reasons",
}
_MANIFEST_KEYS = {
    "protocol", "nonce", "request_challenge", "run_id", "capability_id", "path_id", "topology_generation",
    "topology_sha256", "trace_mode", "exact_required", "boot_id",
    "agent_executable_sha256", "request_sha256", "child_pid", "child_start_time_ticks",
    "wait_status", "graceful", "no_escalation", "descendant_policy",
    "descendant_detected", "semantic_validation", "source_files", "archive_tree_sha256",
    "snapshot_valid", "reasons",
}
_REQUEST_KEYS = {
    "argv", "capability_id", "env", "exact_required", "path_id", "run_id", "run_root",
    "timeout_ms", "topology_generation", "topology_sha256", "trace_mode",
    "request_challenge",
}
_V2_PROTOCOL = "mw_perf.ohos_remote_capture.v2"
_V2_RESULT_PROTOCOL = "mw_perf.ohos_remote_capture.result.v2"
_V2_REQUEST_PROTOCOL = "mw_perf.ohos_remote_capture.request.v2"
_V2_CAPABILITY = "dds.ordinary.nonplain.single_driver.sync_callback"
_V2_WORKLOAD = "dds.ordinary.nonplain.single_driver.sync_callback"
_FAULT_IDS = {"callback_loss", "callback_status", "producer_identity_mismatch", "stage_loss", "stage_status"}
_V2_CONTRACT = "mw_perf.p0a_contract.v8"
_V2_CONTRACT_SHA256 = "c8f76997156ecd6e608f0d5d4a0d756d3e5748e8b21c67895936ee004a614575"
_V2_ATTESTATION = "device_runtime_attestation.json"
_V2_COMPLETION = "device_completion.json"
_V2_RESULT_KEYS = _RESULT_KEYS | {"trust_registration"}
_V2_REQUEST_KEYS = _REQUEST_KEYS | {
    "protocol", "capture_role", "campaign_id", "fault", "workload_id",
    "registration_contract", "runtime_images", "ready_timeout_ms",
    "completion_timeout_ms", "trust_registration",
}
_V2_MANIFEST_KEYS = _MANIFEST_KEYS | {
    "capture_role", "campaign_id", "fault", "workload_id", "registration_contract",
    "runtime_images", "ready_observed", "release_granted", "completion_observed",
    "completion_images", "completion_attested", "evidence_released",
    "ready_attested", "completion_eof_observed",
    "ready_images_sha256", "completion_images_sha256",
    "device_completion_sha256", "device_runtime_attestation_sha256", "trust_registration",
}


class CaptureSemanticError(ValueError):
    """The remote result or snapshot is not a trustworthy producer trace."""


def _fail(message: str) -> None:
    raise CaptureSemanticError(message)


def _strict_json(data: bytes, member_name: str) -> dict[str, Any]:
    def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                _fail(f"duplicate JSON key in {member_name}: {key}")
            result[key] = value
        return result

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=reject_duplicates,
                           parse_constant=lambda value: _fail(f"invalid number in {member_name}: {value}"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CaptureSemanticError(f"invalid JSON in {member_name}") from error
    if not isinstance(value, dict):
        _fail(f"JSON object required in {member_name}")
    return value


def _integer(value: Any, *, minimum: int = 0) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def _canonical_device_path(value: Any, label: str) -> PurePosixPath:
    if (not isinstance(value, str) or not value.startswith("/") or value.startswith("//")
            or "\0" in value or (value != "/" and any(
                part in {"", ".", ".."} for part in value.split("/")[1:]))):
        _fail(f"{label} must be a canonical absolute device path")
    path = PurePosixPath(value)
    if str(path) != value:
        _fail(f"{label} must be a canonical absolute device path")
    return path


def _canonical_local_path(value: str | Path) -> Path:
    try:
        raw = os.fspath(value)
    except TypeError as error:
        raise CaptureSemanticError("local archive path is malformed") from error
    if (not isinstance(raw, str) or not os.path.isabs(raw) or raw.startswith("//")
            or "\0" in raw or os.path.normpath(raw) != raw):
        _fail("local archive path must be canonical and absolute")
    return Path(raw)


def _canonical_request(expected: dict[str, Any]) -> bytes:
    keys = _V2_REQUEST_KEYS if expected.get("protocol") == _V2_REQUEST_PROTOCOL else _REQUEST_KEYS
    if set(expected) != keys:
        _fail("expected request protocol fields mismatch")
    if (not isinstance(expected["argv"], list) or not expected["argv"]
            or not all(isinstance(item, str) for item in expected["argv"])
            or not isinstance(expected["env"], list)
            or not all(isinstance(item, str) for item in expected["env"])
            or not isinstance(expected["exact_required"], bool)
            or not _integer(expected["timeout_ms"], minimum=1)
            or expected["trace_mode"] not in {"off", "sampled", "full"}
            or not all(isinstance(expected[key], str) and expected[key]
                       for key in ("capability_id", "path_id", "run_id", "run_root",
                                   "topology_generation"))
            or not isinstance(expected["topology_sha256"], str)
            or _HEX64.fullmatch(expected["topology_sha256"]) is None
            or not isinstance(expected["request_challenge"], str)
            or re.fullmatch(r"[0-9a-f]{64}", expected["request_challenge"]) is None):
        _fail("expected request is malformed")
    _canonical_device_path(expected["run_root"], "expected run_root")
    if keys == _V2_REQUEST_KEYS:
        _validate_v2_request(expected)
    try:
        return json.dumps(expected, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                          allow_nan=False).encode("ascii")
    except (UnicodeEncodeError, ValueError) as error:
        raise CaptureSemanticError("expected request is not canonical-agent compatible") from error


def _schema_validate(value: Any, filename: str, label: str) -> None:
    try:
        from . import core
    except ImportError:
        import core  # type: ignore[no-redef]
    if core._resource_integrity_errors():
        _fail("trusted collector resources failed integrity verification")
    try:
        raw = (Path(__file__).parent / "schemas" / filename).read_bytes()
        schema = _strict_json(raw, filename)
    except OSError as error:
        raise CaptureSemanticError(f"trusted {label} schema unavailable") from error
    errors = core._schema_errors(value, schema)
    if errors:
        _fail(f"{label} schema mismatch: {errors[0]}")


def _validate_v2_request(request: dict[str, Any]) -> None:
    _schema_validate(request, "ohos-capture-request.schema.v2.json", "v2 request")
    contract = request["registration_contract"]
    registration = request["trust_registration"]
    images = request["runtime_images"]
    image_keys = [(item["role"], item["name"]) for item in images]
    if len(image_keys) != len(set(image_keys)):
        _fail("runtime image role/name pairs must be unique")
    exact = request["capture_role"] == "exact_anchor"
    fault = request["fault"] in _FAULT_IDS
    if ((exact and request["fault"] != "none") or (fault and request["capture_role"] != "fault_runtime_subject")
            or (not exact and not fault)
            or request["capability_id"] != _V2_CAPABILITY
            or request["workload_id"] != _V2_WORKLOAD
            or request["trace_mode"] != "full" or request["exact_required"] is not True
            or contract != {"id": _V2_CONTRACT, "sha256": _V2_CONTRACT_SHA256}
            or set(registration) != {"id", "sha256"}
            or not isinstance(registration["id"], str) or not registration["id"]
            or _HEX64.fullmatch(registration["sha256"]) is None
            or [(item["role"], item["source"]) for item in images]
               != [("sut", "exe"), ("extension", "maps"), ("fastdds", "maps"),
                   ("fastcdr", "maps"), ("cpp_runtime", "maps")]):
        _fail("unsupported v2 capture contract")
    if images[0]["path"] != request["argv"][0]:
        _fail("sut executable path must equal argv[0]")
    if sum(len(item) + 1 for item in request["argv"]) > 1024 * 1024:
        _fail("v2 argv byte limit exceeded")
    env_names: set[str] = set()
    env_bytes = 0
    for literal in request["env"]:
        name, value = literal.split("=", 1)
        if (len(name) > 128 or name.startswith(("MW_PERF_", "IBMW_"))
                or name in env_names or not value):
            _fail("v2 environment allowlist is malformed")
        env_names.add(name)
        env_bytes += len(name) + len(value) + 1
    if env_bytes > 65536:
        _fail("v2 environment byte limit exceeded")
    for item in images:
        if "|" in item["path"]:
            _fail("runtime image path contains the native declaration delimiter")
        _canonical_device_path(item["path"], "runtime image path")
        encoded = "|".join((item["role"], item["name"], item["source"],
                            item["path"], item["sha256"]))
        if len(encoded) > 4096:
            _fail("runtime image declaration byte limit exceeded")


def _octal(field: bytes, width: int, label: str) -> int:
    if len(field) != width or field[-1:] != b"\0" or any(c not in b"01234567" for c in field[:-1]):
        _fail(f"noncanonical tar {label}")
    return int(field[:-1], 8)


def _parse_ustar(data: bytes) -> list[tuple[str, bytes]]:
    if len(data) < 1024 or len(data) % _BLOCK or len(data) > MAX_ARCHIVE_BYTES:
        _fail("invalid archive size")
    entries: list[tuple[str, bytes]] = []
    names: set[str] = set()
    logical_size = 0
    record_count = 0
    offset = 0
    while True:
        if offset + 1024 > len(data):
            _fail("tar terminator missing")
        header = data[offset:offset + _BLOCK]
        if header == b"\0" * _BLOCK:
            if data[offset:] != b"\0" * 1024:
                _fail("tar must end with exactly two zero blocks")
            break
        if len(entries) >= MAX_MEMBERS:
            _fail("archive member limit exceeded")
        if header[257:263] != b"ustar\0" or header[263:265] != b"00":
            _fail("canonical USTAR magic/version required")
        checksum = bytearray(header)
        checksum[148:156] = b" " * 8
        expected_sum = sum(checksum)
        if header[148:156] != f"{expected_sum:06o}\0 ".encode("ascii"):
            _fail("noncanonical tar checksum")
        nul = header[:100].find(b"\0")
        if nul <= 0 or any(header[nul:100]) or any(header[345:512]):
            _fail("noncanonical tar name/prefix")
        try:
            name = header[:nul].decode("ascii")
        except UnicodeDecodeError as error:
            raise CaptureSemanticError("non-ASCII tar member name") from error
        if (name in names or name.startswith(("/", "./")) or "/" in name
                or name in {".", ".."}):
            _fail(f"duplicate or unsafe archive member: {name}")
        names.add(name)
        mode = _octal(header[100:108], 8, "mode")
        uid = _octal(header[108:116], 8, "uid")
        gid = _octal(header[116:124], 8, "gid")
        size = _octal(header[124:136], 12, "size")
        mtime = _octal(header[136:148], 12, "mtime")
        if (mode, uid, gid, mtime) != (0o600, 0, 0, 0):
            _fail(f"noncanonical tar metadata: {name}")
        if header[156:157] != b"0" or any(header[157:257]) or any(header[265:345]):
            _fail(f"tar extensions, links, or devices are forbidden: {name}")
        if size > MAX_MEMBER_BYTES or logical_size > MAX_ARCHIVE_BYTES - size:
            _fail("extracted logical size limit exceeded")
        logical_size += size
        if name.endswith(('.ibct', '.ibce')):
            record_size = 64 if name.endswith('.ibct') else 72
            header_size = 256 if name.endswith('.ibct') else 160
            if size < header_size or (size - header_size) % record_size:
                _fail(f"invalid binary member size: {name}")
            record_count += (size - header_size) // record_size
            if record_count > MAX_RECORDS:
                _fail("parsed record count limit exceeded")
        start = offset + _BLOCK
        end = start + size
        padded = start + ((size + _BLOCK - 1) // _BLOCK) * _BLOCK
        if padded > len(data) or any(data[end:padded]):
            _fail(f"truncated or nonzero-padded tar member: {name}")
        entries.append((name, data[start:end]))
        offset = padded
    expected_names = [_MANIFEST] + sorted((name for name, _ in entries[1:]), key=str.encode)
    if not entries or [name for name, _ in entries] != expected_names:
        _fail("archive member ordering is not canonical")
    return entries


def _read_bound_archive(path: Path, result: dict[str, Any]) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as error:
        raise CaptureSemanticError("archive open failed") from error
    try:
        before = os.fstat(fd)
        named_before = os.stat(path, follow_symlinks=False)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
                or before.st_uid != os.getuid() or stat.S_IMODE(before.st_mode) != 0o600
                or (before.st_dev, before.st_ino) != (named_before.st_dev, named_before.st_ino)
                or before.st_size != result["archive_size"] or before.st_size > MAX_ARCHIVE_BYTES):
            _fail("archive identity or size mismatch")
        chunks: list[bytes] = []
        digest = hashlib.sha256()
        remaining = before.st_size
        while remaining:
            chunk = os.read(fd, min(65536, remaining))
            if not chunk:
                _fail("archive truncated while hashing")
            chunks.append(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
        if os.read(fd, 1):
            _fail("archive grew while hashing")
        after = os.fstat(fd)
        named_after = os.stat(path, follow_symlinks=False)
        identity = lambda value: (value.st_dev, value.st_ino, value.st_size,
                                  value.st_mtime_ns, value.st_ctime_ns)
        if identity(before) != identity(after) or identity(after) != identity(named_after):
            _fail("archive mutated or was replaced")
        if digest.hexdigest() != result["archive_sha256"]:
            _fail("archive SHA256 mismatch")
        return b"".join(chunks)
    except OSError as error:
        raise CaptureSemanticError("archive changed during validation") from error
    finally:
        os.close(fd)


def _validate_result(result: dict[str, Any], *, v2: bool = False) -> None:
    if set(result) != (_V2_RESULT_KEYS if v2 else _RESULT_KEYS):
        _fail("result protocol fields mismatch")
    if v2:
        _schema_validate(result, "ohos-capture-result.schema.v2.json", "v2 result")
    if (result["protocol"] != (_V2_RESULT_PROTOCOL if v2 else "mw_perf.ohos_remote_capture.result.v1")
            or result["snapshot_valid"] is not True
            or result["semantic_validation"] is not False
            or result["reasons"] != []
            or not isinstance(result["archive_path"], str)
            or not _integer(result["archive_size"], minimum=1)
            or result["archive_size"] > MAX_ARCHIVE_BYTES
            or not isinstance(result["archive_sha256"], str)
            or _HEX64.fullmatch(result["archive_sha256"]) is None
            or not _integer(result["child_pid"], minimum=2)
            or not _integer(result["child_start_time_ticks"], minimum=1)):
        _fail("invalid successful result protocol")
    if v2 and (not isinstance(result["trust_registration"], dict)
               or set(result["trust_registration"]) != {"id", "sha256"}
               or not isinstance(result["trust_registration"]["id"], str)
               or not result["trust_registration"]["id"]
               or _HEX64.fullmatch(result["trust_registration"]["sha256"]) is None):
        _fail("invalid v2 result trust registration")


def _materialize_and_validate(files: dict[str, bytes], manifest: dict[str, Any],
                              *, contract_version: str = "mw_perf.p0a_contract.v4") -> dict[str, Any]:
    try:
        from . import core, dds_trace
    except ImportError:
        import core  # type: ignore[no-redef]
        import dds_trace  # type: ignore[no-redef]
    with tempfile.TemporaryDirectory(prefix="mw-perf-ohos-import-") as temporary:
        directory = Path(temporary)
        os.chmod(directory, 0o700)
        dirfd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                        | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0))
        try:
            for name, content in files.items():
                fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                             | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
                             0o600, dir_fd=dirfd)
                try:
                    view = memoryview(content)
                    while view:
                        count = os.write(fd, view)
                        if count <= 0:
                            _fail(f"failed to materialize archive member: {name}")
                        view = view[count:]
                finally:
                    os.close(fd)
            stable_files = dds_trace.scan_directory_fd(dirfd)
            if stable_files != files:
                _fail("materialized source set mismatch")
            if core._resource_integrity_errors():
                _fail("trusted collector resources failed integrity verification")
            contract, contract_raw = core._trusted_p0a_contract(contract_version)
            if (contract_version == _V2_CONTRACT
                    and hashlib.sha256(contract_raw).hexdigest() != _V2_CONTRACT_SHA256):
                _fail("trusted v8 contract digest mismatch")
            expected_identity = {
                "run_id": manifest["run_id"], "capability_id": manifest["capability_id"],
                "path_id": manifest["path_id"], "deployment_mode": "managed",
            }
            run_type = "exact_run" if manifest["exact_required"] else "stage_run"
            measurement_kind = "exact_correlated" if manifest["exact_required"] else "stage_only"
            trace_result = dds_trace.validate_directory_files(
                stable_files, contract["artifact_schemas"], core._schema_errors, expected_identity,
                run_type=run_type, measurement_kind=measurement_kind,
                producer_pid=manifest["child_pid"],
                producer_start_time_ticks=manifest["child_start_time_ticks"],
                topology_generation=manifest["topology_generation"],
                topology_sha256=manifest["topology_sha256"],
                registration_contract_version=contract_version)
            dds_trace.confirm_directory_unchanged(dirfd, stable_files)
            return trace_result
        except (OSError, ValueError, UnicodeError, json.JSONDecodeError, KeyError) as error:
            if isinstance(error, CaptureSemanticError):
                raise
            raise CaptureSemanticError("imported trace semantic validation failed") from error
        finally:
            os.close(dirfd)


def _validate_v2_bindings(files: dict[str, bytes], manifest: dict[str, Any],
                          request: dict[str, Any], result: dict[str, Any]) -> None:
    expected = {
        "capture_role": request["capture_role"], "campaign_id": request["campaign_id"],
        "fault": request["fault"], "workload_id": request["workload_id"],
        "registration_contract": request["registration_contract"],
        "runtime_images": request["runtime_images"],
        "trust_registration": request["trust_registration"],
    }
    if (any(manifest.get(key) != value for key, value in expected.items())
            or manifest["ready_observed"] is not True
            or manifest["release_granted"] is not True
            or manifest["completion_observed"] is not True
            or manifest["completion_attested"] is not True
            or manifest["evidence_released"] is not True
            or manifest["ready_attested"] is not True
            or manifest["completion_eof_observed"] is not True
            or result["trust_registration"] != request["trust_registration"]):
        _fail("v2 role, contract, image, or lifecycle binding mismatch")
    if not {_V2_COMPLETION, _V2_ATTESTATION}.issubset(files):
        _fail("v2 attestation members missing")
    completion_raw, attestation_raw = files[_V2_COMPLETION], files[_V2_ATTESTATION]
    if (manifest["device_completion_sha256"] != hashlib.sha256(completion_raw).hexdigest()
            or manifest["device_runtime_attestation_sha256"] != hashlib.sha256(attestation_raw).hexdigest()):
        _fail("v2 attestation hash mismatch")
    completion = _strict_json(completion_raw, _V2_COMPLETION)
    attestation = _strict_json(attestation_raw, _V2_ATTESTATION)
    _schema_validate(completion, "ohos-device-completion.schema.v2.json", "device completion")
    _schema_validate(attestation, "ohos-runtime-attestation.schema.v2.json", "runtime attestation")
    identity = {
        "request_challenge": request["request_challenge"], "campaign_id": request["campaign_id"],
        "run_id": request["run_id"], "capture_role": request["capture_role"],
        "boot_id": manifest["boot_id"], "child_pid": result["child_pid"],
        "child_start_time_ticks": result["child_start_time_ticks"],
    }
    ready_images = attestation["live_images"]
    completion_images = attestation["completion_images"]
    ready_hash = hashlib.sha256(json.dumps(
        ready_images, separators=(",", ":"), ensure_ascii=True).encode("ascii")).hexdigest()
    completion_hash = hashlib.sha256(json.dumps(
        completion_images, separators=(",", ":"), ensure_ascii=True).encode("ascii")).hexdigest()
    if (any(completion.get(key) != value for key, value in identity.items())
            or completion["ready"] is not True or completion["released"] is not True
            or completion["complete"] is not True
            or completion["completion_attested"] is not True
            or completion["evidence_released"] is not True
            or completion["ready_attested"] is not True
            or completion["completion_eof_observed"] is not True
            or completion["completion_images"] != completion_images
            or any(attestation.get(key) != value for key, value in identity.items())
            or attestation["prelaunch_images"] != request["runtime_images"]
            or attestation["ready_attested"] is not True
            or attestation["completion_attested"] is not True
            or attestation["evidence_released"] is not True
            or attestation["completion_eof_observed"] is not True
            or len(ready_images) != 5 or completion_images != ready_images
            or manifest["completion_images"] != completion_images
            or any(value != ready_hash for value in (
                completion["ready_images_sha256"], attestation["ready_images_sha256"],
                manifest["ready_images_sha256"]))
            or any(value != completion_hash for value in (
                completion["completion_images_sha256"], attestation["completion_images_sha256"],
                manifest["completion_images_sha256"]))):
        _fail("v2 completion or runtime identity replay mismatch")
    for declared, observed in zip(request["runtime_images"], ready_images):
        if (any(observed.get(key) != declared[key] for key in ("role", "name", "source"))
                or observed.get("expected_path") != declared["path"]
                or observed.get("expected_sha256") != declared["sha256"]
                or observed.get("observed_sha256") != declared["sha256"]
                or not _integer(observed.get("observed_device_major"))
                or not _integer(observed.get("observed_device_minor"))
                or not _integer(observed.get("observed_inode"), minimum=1)
                or observed["visible"] is not True or observed["matched"] is not True):
            _fail("v2 live runtime image mismatch")


def _validate_capture_semantics(
    archive_path: str | Path, result: dict[str, Any], expected_request: dict[str, Any]
) -> dict[str, Any]:
    path = _canonical_local_path(archive_path)
    if not isinstance(result, dict):
        _fail("result object required")
    v2 = expected_request.get("protocol") == _V2_REQUEST_PROTOCOL
    _validate_result(result, v2=v2)
    archive_data = _read_bound_archive(path, result)
    return validate_capture_bytes(archive_data, path.name, result, expected_request)


def validate_capture_bytes(archive_data: bytes, archive_name: str, result: dict[str, Any],
                           expected_request: dict[str, Any]) -> dict[str, Any]:
    """Validate already-retained archive bytes without reopening a mutable path."""
    if not isinstance(result, dict):
        _fail("result object required")
    v2 = expected_request.get("protocol") == _V2_REQUEST_PROTOCOL
    _validate_result(result, v2=v2)
    if (not isinstance(archive_data, bytes) or len(archive_data) != result["archive_size"]
            or hashlib.sha256(archive_data).hexdigest() != result["archive_sha256"]):
        _fail("retained archive bytes do not match result")
    request_bytes = _canonical_request(expected_request)
    challenge = expected_request["request_challenge"]
    basename = f"capture-{challenge}.tar"
    device_root = _canonical_device_path(expected_request["run_root"], "expected run_root")
    device_archive = _canonical_device_path(result["archive_path"], "result archive_path")
    if (result["request_challenge"] != challenge
            or archive_name != basename
            or device_archive.name != basename):
        _fail("request challenge archive binding mismatch")
    if device_archive != device_root / basename:
        _fail("archive path is outside the expected run root")
    entries = _parse_ustar(archive_data)
    manifest = _strict_json(entries[0][1], _MANIFEST)
    if set(manifest) != (_V2_MANIFEST_KEYS if v2 else _MANIFEST_KEYS):
        _fail("manifest protocol fields mismatch")
    if v2:
        _schema_validate(manifest, "ohos-capture-manifest.schema.v2.json", "v2 manifest")
    expected_fields = {
        "run_id": expected_request["run_id"],
        "capability_id": expected_request["capability_id"],
        "path_id": expected_request["path_id"],
        "topology_generation": expected_request["topology_generation"],
        "topology_sha256": expected_request["topology_sha256"],
        "trace_mode": expected_request["trace_mode"],
        "exact_required": expected_request["exact_required"],
        "child_pid": result["child_pid"],
        "child_start_time_ticks": result["child_start_time_ticks"],
    }
    if (manifest["protocol"] != (_V2_PROTOCOL if v2 else "mw_perf.ohos_remote_capture.v1")
            or any(manifest.get(key) != value for key, value in expected_fields.items())
            or manifest["request_sha256"] != hashlib.sha256(request_bytes).hexdigest()
            or not isinstance(manifest["agent_executable_sha256"], str)
            or _HEX64.fullmatch(manifest["agent_executable_sha256"]) is None
            or manifest["request_challenge"] != challenge
            or not isinstance(manifest["nonce"], str)
            or re.fullmatch(r"[0-9a-f]{32}", manifest["nonce"]) is None
            or not isinstance(manifest["boot_id"], str) or not manifest["boot_id"]
            or manifest["wait_status"] != 0 or manifest["graceful"] is not True
            or manifest["no_escalation"] is not True
            or manifest["descendant_policy"] != "forbid"
            or manifest["descendant_detected"] is not False
            or manifest["semantic_validation"] is not False
            or manifest["snapshot_valid"] is not True or manifest["reasons"] != []):
        _fail("manifest request, lifecycle, or protocol mismatch")
    files = dict(entries[1:])
    if v2:
        _validate_v2_bindings(files, manifest, expected_request, result)
    declared = manifest["source_files"]
    if not isinstance(declared, list) or len(declared) != len(files):
        _fail("source file inventory mismatch")
    inventory_names: set[str] = set()
    tree = hashlib.sha256()
    for item in declared:
        if (not isinstance(item, dict) or set(item) != {"name", "size", "sha256"}
                or not isinstance(item["name"], str) or item["name"] in inventory_names
                or not _integer(item["size"])
                or not isinstance(item["sha256"], str)
                or _HEX64.fullmatch(item["sha256"]) is None):
            _fail("invalid or duplicate source file inventory")
        inventory_names.add(item["name"])
        content = files.get(item["name"])
        if (content is None or item["size"] != len(content)
                or item["sha256"] != hashlib.sha256(content).hexdigest()):
            _fail(f"source hash mismatch: {item['name']}")
        tree.update(item["name"].encode("utf-8") + b"\0")
        tree.update(str(item["size"]).encode("ascii") + b"\0")
        tree.update(item["sha256"].encode("ascii") + b"\n")
    if inventory_names != set(files) or [item["name"] for item in declared] != sorted(files):
        _fail("source inventory does not exactly match canonical members")
    if (not isinstance(manifest["archive_tree_sha256"], str)
            or manifest["archive_tree_sha256"] != tree.hexdigest()):
        _fail("archive tree hash mismatch")
    trace_files = files if not v2 else {name: content for name, content in files.items()
                                        if name not in {_V2_COMPLETION, _V2_ATTESTATION}}
    if v2 and expected_request.get("capture_role") == "fault_runtime_subject":
        try:
            _materialize_and_validate(trace_files, manifest, contract_version=_V2_CONTRACT)
        except CaptureSemanticError as error:
            return {"semantic_validation": False, "fault_error": str(error), "formal_p0a": False}
        _fail("fault subject trace is semantically valid")
    trace_result = _materialize_and_validate(
        trace_files, manifest, contract_version=_V2_CONTRACT if v2 else "mw_perf.p0a_contract.v4")
    return {
        "semantic_validation": True,
        "imported_semantics": trace_result,
        "formal_p0a": False,
    }


def validate_capture_semantics(
    archive_path: str | Path, result: dict[str, Any], expected_request: dict[str, Any]
) -> dict[str, Any]:
    """Authenticate, import, and semantically validate one remote capture."""
    try:
        return _validate_capture_semantics(archive_path, result, expected_request)
    except MemoryError as error:
        raise CaptureSemanticError("imported trace exceeds memory budget") from error
