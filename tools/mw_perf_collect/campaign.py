"""Deterministic content-addressed companion evidence bundles."""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import secrets
import shutil
import stat
import tarfile
import ctypes
import errno
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping

from . import core
from .core import _canonical_json_bytes, _canonical_relative, _schema_errors
from .json_strict import loads as strict_json_loads


_SCHEMA_ID = "mw_perf.evidence_companion_bundle.v1"
_TREE_DOMAIN = b"mw_perf.archive_tree.v1\0"
_HEX64 = re.compile(r"[0-9a-f]{64}\Z")
_MAX_OBJECT_SIZE = 256 * 1024 * 1024
_MAX_MANIFEST_SIZE = 1024 * 1024
_MAX_PAYLOAD_MEMBERS = 4096
_MAX_PAYLOAD_MEMBER_SIZE = 128 * 1024 * 1024
_MAX_TOTAL_PAYLOAD_SIZE = 128 * 1024 * 1024
_ROLES = {
    "exact_anchor",
    "fault_evidence_tamper",
    "fault_runtime_matrix",
    "rollback_trace_off",
}
_FINAL_ROLE_ORDER = (
    "exact_anchor",
    "fault_evidence_tamper",
    "fault_runtime_matrix",
    "rollback_trace_off",
)
_CAMPAIGN_ROOT_DOMAIN = b"mw_perf.evidence_campaign.root.v1\0"
_REGISTRATION_SET_DOMAIN = b"mw_perf.evidence_campaign.registrations.v1\0"
_CAMPAIGN_ROOT_DOMAIN_V2 = b"mw_perf.evidence_campaign.root.v2\0"
_REGISTRATION_SET_DOMAIN_V2 = b"mw_perf.evidence_campaign.registrations.v2\0"
_METADATA_KEYS = {
    "campaign_id", "role", "companion_id", "run_id", "evidence_contract",
    "root_manifest_sha256", "evidence_manifest_sha256", "build_identity_sha256",
}
_DESCRIPTOR_KEYS = {
    "campaign_id", "role", "companion_id", "object_sha256",
    "bundle_manifest_sha256", "archive_tree_sha256", "run_id", "evidence_contract",
    "root_manifest_sha256", "evidence_manifest_sha256", "build_identity_sha256",
}


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _valid_id(value: Any) -> bool:
    if not isinstance(value, str) or not value or "\x00" in value:
        return False
    try:
        value.encode("utf-8")
    except UnicodeError:
        return False
    return True


def _valid_hash(value: Any) -> bool:
    return isinstance(value, str) and _HEX64.fullmatch(value) is not None


def _validate_metadata(metadata: Any) -> dict[str, Any]:
    if not isinstance(metadata, dict) or set(metadata) != _METADATA_KEYS:
        raise ValueError("companion metadata has unknown or missing fields")
    if any(not _valid_id(metadata[key]) for key in ("campaign_id", "companion_id", "run_id")):
        raise ValueError("companion identifiers must be non-empty strings")
    if metadata["role"] not in _ROLES:
        raise ValueError("invalid companion role")
    contract = metadata["evidence_contract"]
    if (not isinstance(contract, dict) or set(contract) != {"id", "sha256"}
            or not _valid_id(contract["id"]) or not _valid_hash(contract["sha256"])):
        raise ValueError("invalid evidence contract binding")
    for key in ("root_manifest_sha256", "evidence_manifest_sha256", "build_identity_sha256"):
        if not _valid_hash(metadata[key]):
            raise ValueError(f"invalid {key}")
    return {
        "campaign_id": metadata["campaign_id"],
        "role": metadata["role"],
        "companion_id": metadata["companion_id"],
        "run_id": metadata["run_id"],
        "evidence_contract": {
            "id": contract["id"],
            "sha256": contract["sha256"],
        },
        "root_manifest_sha256": metadata["root_manifest_sha256"],
        "evidence_manifest_sha256": metadata["evidence_manifest_sha256"],
        "build_identity_sha256": metadata["build_identity_sha256"],
    }


def _canonical_payload_path(relative: str) -> str:
    if not isinstance(relative, str) or "\x00" in relative:
        raise ValueError("NUL is not permitted in payload paths")
    try:
        relative.encode("utf-8")
    except UnicodeError as exc:
        raise ValueError("payload paths must contain valid Unicode scalars") from exc
    return _canonical_relative(relative)


@dataclass(frozen=True, init=False)
class PayloadSnapshot:
    """A coherent, bounded, immutable mapping of canonical payload paths to bytes."""

    files: Mapping[str, bytes]

    def __init__(self, files: Mapping[str, bytes]) -> None:
        if not isinstance(files, Mapping):
            raise TypeError("snapshot files must be a mapping")
        object.__setattr__(self, "files", MappingProxyType(dict(files)))


def _snapshot_files(snapshot: PayloadSnapshot) -> dict[str, bytes]:
    if not isinstance(snapshot, PayloadSnapshot) or not isinstance(snapshot.files, Mapping):
        raise TypeError("snapshot must be a PayloadSnapshot")
    files: dict[str, bytes] = {}
    total_size = 0
    for relative, data in snapshot.files.items():
        canonical = _canonical_payload_path(relative)
        if canonical != relative or not isinstance(data, bytes):
            raise ValueError("snapshot contains a non-canonical path or mutable content")
        if canonical in files:
            raise ValueError("snapshot contains a duplicate payload path")
        if len(files) >= _MAX_PAYLOAD_MEMBERS:
            raise ValueError("payload member count exceeds limit")
        if len(data) > _MAX_PAYLOAD_MEMBER_SIZE:
            raise ValueError("payload member size exceeds limit")
        total_size += len(data)
        if total_size > _MAX_TOTAL_PAYLOAD_SIZE:
            raise ValueError("total payload size exceeds limit")
        files[canonical] = data
    if not files:
        raise ValueError("payload must contain at least one regular file")
    return files


def _tree_entries(files: Mapping[str, bytes]) -> list[dict[str, Any]]:
    return [
        {"relative_path": relative, "type": "file", "mode": 0o600,
         "size": len(files[relative]), "content_sha256": _hash(files[relative])}
        for relative in sorted(files, key=lambda item: item.encode("utf-8"))
    ]


def _payload_snapshot(payload_root: Path) -> tuple[list[dict[str, Any]], dict[str, bytes]]:
    directory_flags = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                       | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    file_flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    root_fd = -1
    try:
        root_fd = os.open(Path(payload_root), directory_flags)
        root_info = os.fstat(root_fd)
    except OSError as exc:
        if root_fd >= 0:
            os.close(root_fd)
        raise ValueError("payload root is unavailable") from exc
    if not stat.S_ISDIR(root_info.st_mode):
        os.close(root_fd)
        raise ValueError("payload root must be a real directory")

    files: dict[str, bytes] = {}
    total_size = 0

    def identity(value: os.stat_result) -> tuple[int, int, int, int, int]:
        return (value.st_dev, value.st_ino, value.st_size,
                value.st_mtime_ns, value.st_ctime_ns)

    def scan(directory_fd: int, prefix: str, read_files: bool,
             identities: dict[str, tuple[str, tuple[int, int, int, int, int]]]) -> None:
        nonlocal total_size
        try:
            with os.scandir(directory_fd) as iterator:
                members = list(iterator)
            members.sort(key=lambda item: item.name.encode("utf-8"))
        except (OSError, UnicodeError) as exc:
            raise ValueError("payload subtree is unavailable") from exc
        for member in members:
            relative = _canonical_payload_path(
                f"{prefix}/{member.name}" if prefix else member.name)
            child_fd = -1
            try:
                entry_info = member.stat(follow_symlinks=False)
                if stat.S_ISDIR(entry_info.st_mode):
                    child_fd = os.open(member.name, directory_flags, dir_fd=directory_fd)
                    opened = os.fstat(child_fd)
                    if not stat.S_ISDIR(opened.st_mode) or identity(entry_info) != identity(opened):
                        raise ValueError("payload directory changed during traversal")
                    identities[relative] = ("directory", identity(opened))
                    scan(child_fd, relative, read_files, identities)
                elif stat.S_ISREG(entry_info.st_mode):
                    child_fd = os.open(member.name, file_flags, dir_fd=directory_fd)
                    before = os.fstat(child_fd)
                    if not stat.S_ISREG(before.st_mode) or identity(entry_info) != identity(before):
                        raise ValueError("payload member changed during traversal")
                    if before.st_size > _MAX_PAYLOAD_MEMBER_SIZE:
                        raise ValueError("payload member size exceeds limit")
                    if read_files:
                        if len(files) >= _MAX_PAYLOAD_MEMBERS:
                            raise ValueError("payload member count exceeds limit")
                        if total_size + before.st_size > _MAX_TOTAL_PAYLOAD_SIZE:
                            raise ValueError("total payload size exceeds limit")
                        chunks: list[bytes] = []
                        remaining = before.st_size + 1
                        while remaining:
                            chunk = os.read(child_fd, min(1024 * 1024, remaining))
                            if not chunk:
                                break
                            chunks.append(chunk)
                            remaining -= len(chunk)
                        data = b"".join(chunks)
                        after = os.fstat(child_fd)
                        if identity(before) != identity(after) or len(data) != before.st_size:
                            raise ValueError("payload changed while being read")
                        files[relative] = data
                        total_size += len(data)
                    identities[relative] = ("file", identity(before))
                else:
                    raise ValueError("payload contains a symlink or non-regular member")
            except OSError as exc:
                raise ValueError("payload member is unavailable") from exc
            finally:
                if child_fd >= 0:
                    os.close(child_fd)

    first: dict[str, tuple[str, tuple[int, int, int, int, int]]] = {
        "": ("directory", identity(root_info))
    }
    second: dict[str, tuple[str, tuple[int, int, int, int, int]]] = {}
    confirmed: dict[str, tuple[str, tuple[int, int, int, int, int]]] = {}
    try:
        scan(root_fd, "", True, first)
        root_after = os.fstat(root_fd)
        second[""] = ("directory", identity(root_after))
        scan(root_fd, "", False, second)
        root_confirmed = os.fstat(root_fd)
        confirmed[""] = ("directory", identity(root_confirmed))
        scan(root_fd, "", False, confirmed)
    finally:
        os.close(root_fd)
    if first != second or second != confirmed:
        raise ValueError("payload changed during snapshot")
    if not files:
        raise ValueError("payload must contain at least one regular file")
    entries = _tree_entries(files)
    return entries, files


def capture_payload_snapshot(payload_root: Path) -> PayloadSnapshot:
    """Capture one coherent payload byte snapshot under the companion limits."""
    _entries, files = _payload_snapshot(Path(payload_root))
    return PayloadSnapshot(files)


def _same_identity(left: os.stat_result, right: os.stat_result) -> bool:
    return (left.st_dev, left.st_ino) == (right.st_dev, right.st_ino)


def _publication_checkpoint() -> None:
    """Test hook for races immediately before final publication validation."""


def _revalidate_store_directory(parent_fd: int, name: str, child_fd: int) -> None:
    try:
        path_info = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        descriptor_info = os.fstat(child_fd)
    except OSError as exc:
        raise ValueError("object store changed during publication") from exc
    if (not stat.S_ISDIR(path_info.st_mode)
            or not stat.S_ISDIR(descriptor_info.st_mode)
            or not _same_identity(path_info, descriptor_info)):
        raise ValueError("object store changed during publication")


def _revalidate_publication(root: Path, root_fd: int, sha_fd: int, shard_fd: int,
                            shard_name: str, target_name: str, digest: str,
                            data: bytes, *, sync_object: bool) -> None:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    object_fd = -1
    try:
        root_path_info = os.stat(root, follow_symlinks=False)
        root_info = os.fstat(root_fd)
        if (not stat.S_ISDIR(root_path_info.st_mode) or not stat.S_ISDIR(root_info.st_mode)
                or not _same_identity(root_path_info, root_info)):
            raise ValueError("object store changed during publication")
        _revalidate_store_directory(root_fd, "sha256", sha_fd)
        _revalidate_store_directory(sha_fd, shard_name, shard_fd)

        object_fd = os.open(target_name, flags, dir_fd=shard_fd)
        before = os.fstat(object_fd)
        if (not stat.S_ISREG(before.st_mode) or stat.S_IMODE(before.st_mode) != 0o600
                or before.st_size != len(data)):
            raise ValueError("published object changed during publication")
        chunks: list[bytes] = []
        remaining = len(data) + 1
        while remaining:
            chunk = os.read(object_fd, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        actual = b"".join(chunks)
        after = os.fstat(object_fd)
        target_info = os.stat(target_name, dir_fd=shard_fd, follow_symlinks=False)
        if (not _same_identity(before, after) or not _same_identity(after, target_info)
                or actual != data or _hash(actual) != digest):
            raise ValueError("published object changed during publication")
        if sync_object:
            os.fsync(object_fd)

        _revalidate_store_directory(root_fd, "sha256", sha_fd)
        _revalidate_store_directory(sha_fd, shard_name, shard_fd)
        root_after = os.stat(root, follow_symlinks=False)
        if not stat.S_ISDIR(root_after.st_mode) or not _same_identity(root_after, root_info):
            raise ValueError("object store changed during publication")
    except ValueError:
        raise
    except OSError as exc:
        raise ValueError("published object changed during publication") from exc
    finally:
        if object_fd >= 0:
            os.close(object_fd)


def _open_store_directory(parent_fd: int, name: str) -> int:
    flags = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
             | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    created = False
    try:
        os.mkdir(name, mode=0o700, dir_fd=parent_fd)
        created = True
    except FileExistsError:
        pass
    try:
        child_fd = os.open(name, flags, dir_fd=parent_fd)
    except OSError as exc:
        raise ValueError("object store component is not a directory") from exc
    if created:
        try:
            os.fsync(child_fd)
            os.fsync(parent_fd)
        except BaseException:
            os.close(child_fd)
            raise
    return child_fd


def _create_store_root(root: Path, directory_flags: int) -> None:
    missing: list[Path] = []
    current = root
    while True:
        try:
            current.lstat()
            break
        except FileNotFoundError:
            missing.append(current)
            if current == current.parent:
                break
            current = current.parent
    for directory in reversed(missing):
        created = False
        try:
            directory.mkdir(mode=0o700)
            created = True
        except FileExistsError:
            pass
        if created:
            child_fd = parent_fd = -1
            try:
                child_fd = os.open(directory, directory_flags)
                parent_fd = os.open(directory.parent, directory_flags)
                os.fsync(child_fd)
                os.fsync(parent_fd)
            finally:
                if parent_fd >= 0:
                    os.close(parent_fd)
                if child_fd >= 0:
                    os.close(child_fd)


def compute_archive_tree(payload_root: Path) -> dict[str, Any]:
    """Hash the canonical regular-file tree beneath ``payload_root``."""
    entries, _ = _payload_snapshot(Path(payload_root))
    digest = _hash(_TREE_DOMAIN + _canonical_json_bytes(entries))
    return {"entries": entries, "sha256": digest}


def compute_snapshot_archive_tree(snapshot: PayloadSnapshot, prefix: str = "") -> dict[str, Any]:
    """Hash all files, or one canonical subtree, from an immutable snapshot."""
    files = _snapshot_files(snapshot)
    if prefix:
        canonical_prefix = _canonical_payload_path(prefix)
        marker = canonical_prefix + "/"
        files = {relative[len(marker):]: data for relative, data in files.items()
                 if relative.startswith(marker)}
        if not files:
            raise ValueError("snapshot prefix contains no regular files")
    entries = _tree_entries(files)
    return {"entries": entries,
            "sha256": _hash(_TREE_DOMAIN + _canonical_json_bytes(entries))}


def _tar_info(name: str, size: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size = size
    info.mode = 0o600
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mtime = 0
    info.type = tarfile.REGTYPE
    return info


def _tar_bytes(manifest_bytes: bytes, files: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    try:
        with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            archive.addfile(_tar_info("bundle_manifest.json", len(manifest_bytes)),
                            io.BytesIO(manifest_bytes))
            for relative in sorted(files, key=lambda item: item.encode("utf-8")):
                name = "payload/" + _canonical_payload_path(relative)
                data = files[relative]
                archive.addfile(_tar_info(name, len(data)), io.BytesIO(data))
    except (ValueError, UnicodeError) as exc:
        raise ValueError("payload path cannot be represented safely in USTAR") from exc
    return output.getvalue()


def _manifest(metadata: dict[str, Any], tree_sha256: str, count: int) -> dict[str, Any]:
    return {
        "schema": _SCHEMA_ID,
        "campaign_id": metadata["campaign_id"],
        "role": metadata["role"],
        "companion_id": metadata["companion_id"],
        "run_id": metadata["run_id"],
        "evidence_contract": dict(metadata["evidence_contract"]),
        "root_manifest_sha256": metadata["root_manifest_sha256"],
        "evidence_manifest_sha256": metadata["evidence_manifest_sha256"],
        "build_identity_sha256": metadata["build_identity_sha256"],
        "archive_tree_sha256": tree_sha256,
        "payload_member_count": count,
    }


def _descriptor(manifest: dict[str, Any], object_sha256: str,
                manifest_sha256: str) -> dict[str, Any]:
    return {
        "campaign_id": manifest["campaign_id"],
        "role": manifest["role"],
        "companion_id": manifest["companion_id"],
        "object_sha256": object_sha256,
        "bundle_manifest_sha256": manifest_sha256,
        "archive_tree_sha256": manifest["archive_tree_sha256"],
        "run_id": manifest["run_id"],
        "evidence_contract": dict(manifest["evidence_contract"]),
        "root_manifest_sha256": manifest["root_manifest_sha256"],
        "evidence_manifest_sha256": manifest["evidence_manifest_sha256"],
        "build_identity_sha256": manifest["build_identity_sha256"],
    }


def _store_object(object_store: Path, digest: str, data: bytes) -> Path:
    root = Path(object_store)
    directory_flags = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                       | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    _create_store_root(root, directory_flags)
    root_fd = sha_fd = parent_fd = root_parent_fd = -1
    try:
        root_fd = os.open(root, directory_flags)
        root_info = os.fstat(root_fd)
    except OSError as exc:
        if root_fd >= 0:
            os.close(root_fd)
        raise ValueError("object store must be a real directory") from exc
    target_name = f"{digest}.tar"
    target = root / "sha256" / digest[:2] / target_name

    def accept_existing() -> None:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            existing_fd = os.open(target_name, flags, dir_fd=parent_fd)
            try:
                info = os.fstat(existing_fd)
                if not stat.S_ISREG(info.st_mode) or info.st_size != len(data):
                    raise ValueError("content-addressed object collision")
                offset = 0
                remaining = len(data)
                while remaining:
                    chunk = os.read(existing_fd, min(1024 * 1024, remaining))
                    if not chunk or chunk != data[offset:offset + len(chunk)]:
                        raise ValueError("content-addressed object collision")
                    offset += len(chunk)
                    remaining -= len(chunk)
                if os.read(existing_fd, 1):
                    raise ValueError("content-addressed object collision")
            finally:
                os.close(existing_fd)
        except (OSError, ValueError) as exc:
            if isinstance(exc, ValueError):
                raise
            raise ValueError("content-addressed object collision") from exc
    try:
        try:
            root_parent_fd = os.open(root.parent, directory_flags)
        except OSError:
            root_parent_fd = -1
        sha_fd = _open_store_directory(root_fd, "sha256")
        parent_fd = _open_store_directory(sha_fd, digest[:2])
        published = False
        try:
            accept_existing()
            published = True
        except ValueError as exc:
            try:
                os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise exc

        if not published:
            temporary_name = ""
            fd = -1
            for _ in range(128):
                candidate = f".{digest}.{secrets.token_hex(8)}.tmp"
                try:
                    fd = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                 | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
                                 0o600, dir_fd=parent_fd)
                    temporary_name = candidate
                    break
                except FileExistsError:
                    continue
            if fd < 0:
                raise OSError("unable to allocate private object name")
            try:
                view = memoryview(data)
                while view:
                    written = os.write(fd, view)
                    if written <= 0:
                        raise OSError("object write made no progress")
                    view = view[written:]
                os.fsync(fd)
                os.close(fd)
                fd = -1
                try:
                    os.link(temporary_name, target_name, src_dir_fd=parent_fd,
                            dst_dir_fd=parent_fd, follow_symlinks=False)
                except FileExistsError:
                    os.unlink(temporary_name, dir_fd=parent_fd)
                    temporary_name = ""
                    accept_existing()
                else:
                    os.unlink(temporary_name, dir_fd=parent_fd)
                    temporary_name = ""
            finally:
                if fd >= 0:
                    os.close(fd)
                if temporary_name:
                    try:
                        os.unlink(temporary_name, dir_fd=parent_fd)
                    except FileNotFoundError:
                        pass

        _revalidate_publication(root, root_fd, sha_fd, parent_fd, digest[:2],
                                target_name, digest, data, sync_object=True)
        os.fsync(parent_fd)
        os.fsync(sha_fd)
        os.fsync(root_fd)
        if root_parent_fd >= 0:
            os.fsync(root_parent_fd)
        _publication_checkpoint()
        _revalidate_publication(root, root_fd, sha_fd, parent_fd, digest[:2],
                                target_name, digest, data, sync_object=False)
        return target
    finally:
        if root_parent_fd >= 0:
            os.close(root_parent_fd)
        if parent_fd >= 0:
            os.close(parent_fd)
        if sha_fd >= 0:
            os.close(sha_fd)
        os.close(root_fd)


def build_companion_object(payload_root: Path, object_store: Path,
                           metadata: dict[str, Any]) -> dict[str, Any]:
    """Build and exclusively store one deterministic companion object."""
    normalized = _validate_metadata(metadata)
    snapshot = capture_payload_snapshot(Path(payload_root))
    return build_companion_object_from_snapshot(snapshot, object_store, normalized)


def build_companion_object_from_snapshot(snapshot: PayloadSnapshot, object_store: Path,
                                         metadata: dict[str, Any]) -> dict[str, Any]:
    """Build an object from the exact bytes in one immutable payload snapshot."""
    normalized = _validate_metadata(metadata)
    files = _snapshot_files(snapshot)
    entries = _tree_entries(files)
    tree_sha256 = _hash(_TREE_DOMAIN + _canonical_json_bytes(entries))
    manifest = _manifest(normalized, tree_sha256, len(entries))
    manifest_bytes = _canonical_json_bytes(manifest, newline=True)
    data = _tar_bytes(manifest_bytes, files)
    if len(data) > _MAX_OBJECT_SIZE:
        raise ValueError("companion object size exceeds limit")
    object_sha256 = _hash(data)
    _store_object(Path(object_store), object_sha256, data)
    return _descriptor(manifest, object_sha256, _hash(manifest_bytes))


def _result(codes: set[str], descriptor: dict[str, Any] | None,
            manifest: dict[str, Any] | None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "valid": not codes,
        "failure_codes": sorted(codes),
        "descriptor": descriptor,
    }
    if manifest is not None:
        result["bundle_manifest"] = manifest
    return result


def validate_companion_object(object_path: Path,
                              expected_descriptor: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate one companion tar without extracting it."""
    if expected_descriptor is not None and not isinstance(expected_descriptor, dict):
        raise TypeError("expected_descriptor must be an object or None")
    path = Path(object_path)
    codes: set[str] = set()
    manifest: dict[str, Any] | None = None
    descriptor: dict[str, Any] | None = None
    object_fd = -1
    try:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        object_fd = os.open(path, flags)
        info = os.fstat(object_fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > _MAX_OBJECT_SIZE:
            return _result({"CAMPAIGN_OBJECT_SIZE"}, None, None)
        chunks: list[bytes] = []
        remaining = _MAX_OBJECT_SIZE + 1
        while remaining:
            chunk = os.read(object_fd, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        after = os.fstat(object_fd)
        identity = lambda item: (item.st_dev, item.st_ino, item.st_size,
                                 item.st_mtime_ns, item.st_ctime_ns)
        if len(raw) > _MAX_OBJECT_SIZE:
            return _result({"CAMPAIGN_OBJECT_SIZE"}, None, None)
        if len(raw) != info.st_size or identity(info) != identity(after):
            return _result({"CAMPAIGN_OBJECT_READ"}, None, None)
    except (OSError, UnicodeError):
        return _result({"CAMPAIGN_OBJECT_READ"}, None, None)
    finally:
        if object_fd >= 0:
            os.close(object_fd)
    object_sha256 = _hash(raw)
    if (path.name != f"{object_sha256}.tar" or path.parent.name != object_sha256[:2]
            or path.parent.parent.name != "sha256"):
        codes.add("CAMPAIGN_OBJECT_NAME")

    members: list[tarfile.TarInfo]
    payloads: dict[str, bytes] = {}
    manifest_bytes: bytes | None = None
    try:
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
            members = []
            total_payload_size = 0
            for member in archive:
                members.append(member)
                if len(members) > _MAX_PAYLOAD_MEMBERS + 1:
                    return _result({"CAMPAIGN_TAR_MEMBER_LIMIT"}, None, None)
                if member.size < 0 or member.size > _MAX_PAYLOAD_MEMBER_SIZE:
                    codes.add("CAMPAIGN_TAR_MEMBER_SIZE")
                if member.name == "bundle_manifest.json" and member.size > _MAX_MANIFEST_SIZE:
                    codes.add("CAMPAIGN_MANIFEST_SIZE")
                if member.name.startswith("payload/") and member.size >= 0:
                    total_payload_size += member.size
                    if total_payload_size > _MAX_TOTAL_PAYLOAD_SIZE:
                        codes.add("CAMPAIGN_TAR_TOTAL_SIZE")
            names = [item.name for item in members]
            if len(names) != len(set(names)):
                codes.add("CAMPAIGN_TAR_DUPLICATE")
            for member in members:
                if member.size < 0 or member.size > _MAX_PAYLOAD_MEMBER_SIZE:
                    continue
                if member.name == "bundle_manifest.json" and member.size > _MAX_MANIFEST_SIZE:
                    continue
                if member.name.startswith("payload/") and total_payload_size > _MAX_TOTAL_PAYLOAD_SIZE:
                    continue
                try:
                    canonical_name = _canonical_payload_path(member.name)
                except (TypeError, ValueError):
                    codes.add("CAMPAIGN_TAR_PATH")
                    continue
                if not member.isreg():
                    codes.add("CAMPAIGN_TAR_TYPE")
                    continue
                stream = archive.extractfile(member)
                if stream is None:
                    codes.add("CAMPAIGN_TAR_READ")
                    continue
                data = stream.read()
                if canonical_name == "bundle_manifest.json":
                    if manifest_bytes is not None:
                        codes.add("CAMPAIGN_TAR_DUPLICATE")
                    manifest_bytes = data
                elif canonical_name.startswith("payload/"):
                    relative = canonical_name.removeprefix("payload/")
                    try:
                        _canonical_payload_path(relative)
                    except ValueError:
                        codes.add("CAMPAIGN_TAR_PATH")
                        continue
                    if relative in payloads:
                        codes.add("CAMPAIGN_TAR_DUPLICATE")
                    payloads[relative] = data
                else:
                    codes.add("CAMPAIGN_TAR_EXTRA")
    except (tarfile.TarError, OSError, EOFError, ValueError, UnicodeError):
        return _result(codes | {"CAMPAIGN_TAR_FORMAT"}, None, None)

    if manifest_bytes is None:
        return _result(codes | {"CAMPAIGN_MANIFEST_MISSING"}, None, None)
    try:
        value = strict_json_loads(manifest_bytes)
        if isinstance(value, dict):
            manifest = value
        else:
            codes.add("CAMPAIGN_MANIFEST_SCHEMA")
    except (ValueError, UnicodeError, json.JSONDecodeError):
        codes.add("CAMPAIGN_MANIFEST_JSON")
    if manifest is None:
        return _result(codes, None, None)

    try:
        schema = strict_json_loads(
            (Path(__file__).parent / "schemas/evidence-companion-bundle.schema.v1.json").read_bytes())
        if not isinstance(schema, dict) or _schema_errors(manifest, schema):
            codes.add("CAMPAIGN_MANIFEST_SCHEMA")
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError):
        codes.add("CAMPAIGN_SCHEMA_UNAVAILABLE")
    try:
        canonical_manifest = _canonical_json_bytes(manifest, newline=True)
    except (TypeError, ValueError, UnicodeError):
        return _result(codes | {"CAMPAIGN_MANIFEST_UNICODE"}, None, manifest)
    if manifest_bytes != canonical_manifest:
        codes.add("CAMPAIGN_MANIFEST_CANONICAL")

    try:
        entries = [
            {"relative_path": relative, "type": "file", "mode": 0o600, "size": len(data),
             "content_sha256": _hash(data)}
            for relative, data in sorted(payloads.items(), key=lambda item: item[0].encode("utf-8"))
        ]
        tree_sha256 = _hash(_TREE_DOMAIN + _canonical_json_bytes(entries))
    except (TypeError, ValueError, UnicodeError):
        return _result(codes | {"CAMPAIGN_TAR_PATH"}, None, manifest)
    if not payloads:
        codes.add("CAMPAIGN_PAYLOAD_EMPTY")
    if manifest.get("archive_tree_sha256") != tree_sha256:
        codes.add("CAMPAIGN_ARCHIVE_TREE")
    if manifest.get("payload_member_count") != len(payloads):
        codes.add("CAMPAIGN_PAYLOAD_COUNT")

    expected_names = ["bundle_manifest.json"] + ["payload/" + item["relative_path"] for item in entries]
    actual_names = [item.name for item in members]
    if actual_names != expected_names:
        codes.add("CAMPAIGN_TAR_ORDER")
    if any(item.mode != 0o600 or item.uid != 0 or item.gid != 0 or item.uname != ""
           or item.gname != "" or item.mtime != 0 for item in members):
        codes.add("CAMPAIGN_TAR_METADATA")
    try:
        canonical_tar = _tar_bytes(canonical_manifest, payloads)
        if raw != canonical_tar:
            codes.add("CAMPAIGN_TAR_CANONICAL")
    except ValueError:
        codes.add("CAMPAIGN_TAR_PATH")

    manifest_sha256 = _hash(manifest_bytes)
    required_manifest_keys = _METADATA_KEYS | {
        "schema", "archive_tree_sha256", "payload_member_count",
    }
    if (set(manifest) == required_manifest_keys and manifest.get("schema") == _SCHEMA_ID
            and all(key in manifest for key in required_manifest_keys)):
        metadata = {key: manifest[key] for key in _METADATA_KEYS}
        try:
            _validate_metadata(metadata)
            if (_valid_hash(manifest.get("archive_tree_sha256"))
                    and isinstance(manifest.get("payload_member_count"), int)
                    and not isinstance(manifest.get("payload_member_count"), bool)
                    and manifest["payload_member_count"] >= 1):
                descriptor = _descriptor(manifest, object_sha256, manifest_sha256)
        except (TypeError, ValueError):
            pass
    if descriptor is None:
        codes.add("CAMPAIGN_DESCRIPTOR_BINDING")
    elif expected_descriptor is not None:
        if set(expected_descriptor) != _DESCRIPTOR_KEYS or expected_descriptor != descriptor:
            codes.add("CAMPAIGN_EXPECTED_DESCRIPTOR")
    return _result(codes, descriptor, manifest)


def _campaign_schema(name: str) -> dict[str, Any]:
    try:
        value = strict_json_loads((Path(__file__).parent / "schemas" / name).read_bytes())
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"campaign schema unavailable: {name}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"campaign schema is not an object: {name}")
    return value


def _validate_campaign_document(value: dict[str, Any], schema_name: str) -> None:
    errors = _schema_errors(value, _campaign_schema(schema_name))
    if errors:
        raise ValueError(f"campaign document failed {schema_name}: {errors[0]}")


def _checked_object_path(object_store: Path, digest: str) -> Path:
    """Resolve a digest while rejecting symlinked object-store components."""
    if not _valid_hash(digest):
        raise ValueError("invalid companion object_sha256")
    root = Path(object_store)
    components = (root, root / "sha256", root / "sha256" / digest[:2])
    for component in components:
        try:
            info = component.lstat()
        except OSError as exc:
            raise ValueError("object store component is unavailable") from exc
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError("object store component must be a real directory")
    return components[-1] / f"{digest}.tar"


def _read_canonical_payload_object(path: Path) -> dict[str, bytes]:
    """Read an already generically-validated tar into bounded immutable bytes."""
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = -1
    try:
        fd = os.open(path, flags)
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size > _MAX_OBJECT_SIZE:
            raise ValueError("object is not a bounded regular file")
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
        linked = os.stat(path, follow_symlinks=False)
        identity = lambda item: (item.st_dev, item.st_ino, item.st_size,
                                 item.st_mtime_ns, item.st_ctime_ns)
        if len(raw) != before.st_size or identity(before) != identity(after) \
                or identity(after) != identity(linked):
            raise ValueError("object changed while reading")
    except OSError as exc:
        raise ValueError("object is unavailable") from exc
    finally:
        if fd >= 0:
            os.close(fd)
    if _hash(raw) != path.stem:
        raise ValueError("object hash changed while reading")
    payloads: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
        for member in archive:
            if member.isreg() and member.name.startswith("payload/"):
                stream = archive.extractfile(member)
                if stream is None:
                    raise ValueError("payload member is unreadable")
                relative = _canonical_payload_path(member.name.removeprefix("payload/"))
                payloads[relative] = stream.read()
    return payloads


def _payload_json(payloads: Mapping[str, bytes], name: str, *, canonical: bool = True) -> dict[str, Any]:
    data = payloads[name]
    value = strict_json_loads(data)
    if not isinstance(value, dict) or (canonical and data != _canonical_json_bytes(value, newline=True)):
        raise ValueError(f"{name} is not a valid JSON object")
    return value


def _is_final_campaign_seal(name: str) -> bool:
    basename = name.rsplit("/", 1)[-1].lower()
    return (basename == "campaign_seal.json"
            or re.fullmatch(r"(?:.*[_-])?final[_-]seal(?:\.json)?", basename) is not None)


def _validate_trusted_evidence_payload(
        payloads: Mapping[str, bytes], evidence: Mapping[str, Any], *,
        expected_version: str, expected_run_type: str, expected_run_id: str,
        allowed_incomplete_paths: set[str] | frozenset[str] = frozenset()) -> bytes:
    """Validate a manifest against the complete trusted contract declaration."""
    trusted, trusted_raw = core._trusted_p0a_contract(expected_version)
    contract_raw = payloads.get("registration/contract.json")
    if contract_raw != trusted_raw:
        raise ValueError("archived P0a contract is not trusted")
    if (not isinstance(evidence, dict) or not core._p0a_schema_valid(evidence)
            or evidence.get("registration_contract_version") != expected_version
            or evidence.get("registration_contract_sha256") != _hash(trusted_raw)
            or evidence.get("registration_contract_ref") != "registration/contract.json"
            or evidence.get("run_id") != expected_run_id
            or evidence.get("run_type") != expected_run_type
            or evidence.get("observer") is not False
            or evidence.get("deployment_mode") != "managed"
            or evidence.get("measurement_kind") != "exact_correlated"):
        raise ValueError("P0a evidence identity is invalid")
    declarations = core._p0a_declarations(
        trusted, expected_run_type, False, "exact_correlated", "managed")
    expected: dict[str, dict[str, Any]] = {}
    directory: dict[str, Any] | None = None
    for declaration in declarations:
        relative = declaration["relative_path"]
        if relative.endswith("/"):
            directory = declaration
        else:
            expected[relative] = declaration
    if directory is not None:
        prefix = directory["relative_path"]
        for relative in payloads:
            if relative.startswith(prefix) and relative not in expected:
                expected[relative] = core._directory_file_declaration(directory, relative)
    allowed_incomplete = set(allowed_incomplete_paths)
    if not allowed_incomplete.issubset(expected):
        raise ValueError("P0a incomplete declaration set is invalid")
    entries = evidence.get("entries")
    if not isinstance(entries, list) or any(not isinstance(item, dict) for item in entries):
        raise ValueError("P0a evidence entries are invalid")
    paths = [item.get("relative_path") for item in entries]
    if (len(paths) != len(set(paths)) or set(paths) != set(expected)
            or paths != sorted(paths, key=lambda item: item.encode("utf-8")
                               if isinstance(item, str) else b"")):
        raise ValueError("P0a evidence declaration set is incomplete")
    for entry in entries:
        declaration = expected[entry["relative_path"]]
        expected_finalization = ("not_applicable"
                                 if declaration["status"] == "not_applicable"
                                 else declaration.get("finalization"))
        if (entry.get("status") != declaration["status"]
                or entry.get("schema_version") != declaration.get("schema_version")
                or (entry["relative_path"] in allowed_incomplete
                    and declaration["status"] != "required")
                or (entry.get("finalization") != "incomplete"
                    if entry["relative_path"] in allowed_incomplete
                    else entry.get("finalization") != expected_finalization)):
            raise ValueError("P0a evidence declaration binding is invalid")
        if declaration["status"] == "not_applicable":
            if (entry.get("sha256") is not None or entry.get("record_count") != 0
                    or entry.get("reason") != declaration.get("reason")):
                raise ValueError("P0a conditional evidence binding is invalid")
        else:
            data = payloads.get(entry["relative_path"])
            if entry["relative_path"] in allowed_incomplete:
                if (declaration["status"] != "required" or entry.get("record_count") != 0
                        or entry.get("sha256") is not None or data is not None):
                    raise ValueError("P0a incomplete evidence binding is invalid")
            elif (entry.get("finalization") != "complete" or data is None
                  or entry.get("sha256") != _hash(data)):
                raise ValueError("P0a evidence artifact hash is invalid")
    required = [item for item in entries if item.get("status") == "required"]
    conditional = [item for item in entries if item.get("status") != "required"]
    if evidence.get("required") != required or evidence.get("conditional") != conditional:
        raise ValueError("P0a evidence groups are incomplete")
    return trusted_raw


def validate_exact_anchor_companion(
        object_path: Path, expected_descriptor: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate the exact-anchor semantics from the immutable archive payload."""
    if expected_descriptor is None:
        return {"valid": False, "failure_codes": ["EXACT_ANCHOR_EXPECTED_DESCRIPTOR"],
                "descriptor": None}
    generic = validate_companion_object(Path(object_path), expected_descriptor)
    codes = set(generic.get("failure_codes", []))
    if codes:
        return {"valid": False, "failure_codes": sorted(codes),
                "descriptor": generic.get("descriptor")}
    try:
        payloads = _read_canonical_payload_object(Path(object_path))
        required = {
            "manifest.json", "validation.json", "capability/p0a_evidence_manifest.json",
            "registration/contract.json", "registration/resolution_declarations.json",
            "capability/resolution.json",
        }
        if not required.issubset(payloads) or any(_is_final_campaign_seal(name)
                                                   for name in payloads):
            raise ValueError("exact anchor payload set is invalid")
        manifest = _payload_json(payloads, "manifest.json", canonical=False)
        validation = _payload_json(payloads, "validation.json", canonical=False)
        evidence = _payload_json(payloads, "capability/p0a_evidence_manifest.json", canonical=False)
        contract = _payload_json(payloads, "registration/contract.json", canonical=False)
        declarations = _payload_json(payloads, "registration/resolution_declarations.json", canonical=False)
        resolution = _payload_json(payloads, "capability/resolution.json", canonical=False)
        for value in (manifest, validation, evidence, contract, declarations, resolution):
            if not isinstance(value, dict):
                raise ValueError("exact anchor payload schema is invalid")
        run_id = expected_descriptor.get("run_id")
        if not isinstance(run_id, str):
            raise ValueError("exact anchor run binding is invalid")
        trusted_raw = _validate_trusted_evidence_payload(
            payloads, evidence, expected_version="mw_perf.p0a_contract.v8",
            expected_run_type="exact_run", expected_run_id=run_id,
            allowed_incomplete_paths=frozenset({
                "validation/fault_injection.json", "validation/rollback.json"}))
        if (manifest.get("run_id") != run_id or evidence.get("run_id") != run_id
                or declarations.get("run_id") != run_id or resolution.get("run_id") != run_id):
            raise ValueError("exact anchor run binding is invalid")
        if (manifest.get("candidate_only") is not True
                or evidence.get("registration_contract_version") != "mw_perf.p0a_contract.v8"
                or manifest.get("metric_availability") != "unsupported"
                or evidence.get("measurement_kind") != "exact_correlated"
                or validation.get("valid_for_comparison") is not False
                or validation.get("p0a_evidence", {}).get("valid") is not False
                or validation.get("p0a_evidence", {}).get("failure_codes") != ["P0A_ARTIFACT"]):
            raise ValueError("exact anchor classification is invalid")
        if (expected_descriptor.get("role") != "exact_anchor"
                or expected_descriptor.get("evidence_contract") != {
                    "id": "mw_perf.p0a_contract.v8", "sha256": _hash(trusted_raw)}):
            raise ValueError("exact anchor contract binding is invalid")
        if (expected_descriptor["root_manifest_sha256"] != _hash(payloads["manifest.json"])
                or expected_descriptor["evidence_manifest_sha256"] !=
                _hash(payloads["capability/p0a_evidence_manifest.json"])):
            raise ValueError("exact anchor manifest binding is invalid")
        descriptor = generic.get("descriptor")
        if not isinstance(descriptor, dict):
            raise ValueError("exact anchor descriptor is unavailable")
        return {"valid": True, "failure_codes": [], "descriptor": descriptor,
                "run_id": run_id, "root_manifest_sha256": descriptor["root_manifest_sha256"],
                "evidence_manifest_sha256": descriptor["evidence_manifest_sha256"],
                "evidence_contract": descriptor["evidence_contract"],
                "build_identity_sha256": descriptor["build_identity_sha256"]}
    except (KeyError, OSError, tarfile.TarError, EOFError, ValueError, TypeError,
            UnicodeError, json.JSONDecodeError):
        return {"valid": False, "failure_codes": ["EXACT_ANCHOR_PAYLOAD"],
                "descriptor": generic.get("descriptor")}


def _specialized_companion(role: str, path: Path, descriptor: dict[str, Any],
                           *, object_store: Path | None = None) -> dict[str, Any]:
    if role == "exact_anchor":
        if descriptor.get("evidence_contract", {}).get("id") == "mw_perf.p0a_contract.v9":
            from .formal_anchor import validate_formal_exact_anchor_companion
            return validate_formal_exact_anchor_companion(path, descriptor,
                                                         object_store=object_store)
        return validate_exact_anchor_companion(path, descriptor)
    if role == "fault_evidence_tamper":
        from .fault_evaluation import validate_evidence_tamper_companion
        return validate_evidence_tamper_companion(path, descriptor)
    if role == "fault_runtime_matrix":
        from .runtime_fault_evaluation import validate_runtime_fault_companion
        return validate_runtime_fault_companion(path, descriptor)
    from .rollback_evaluation import validate_trace_off_rollback_companion
    return validate_trace_off_rollback_companion(path, descriptor)


def _validate_role_bindings(results: Mapping[str, Mapping[str, Any]]) -> None:
    anchor = results["exact_anchor"]
    identity = {key: anchor.get(key) for key in
                ("run_id", "root_manifest_sha256", "evidence_manifest_sha256", "evidence_contract")}
    for role in _FINAL_ROLE_ORDER[1:]:
        result = results[role]
        descriptor = result.get("descriptor")
        if not isinstance(descriptor, Mapping):
            raise ValueError(f"{role} companion descriptor is unavailable")
        candidate_binding = result.get("anchor")
        binding: Mapping[str, Any] = candidate_binding if isinstance(
            candidate_binding, Mapping) else result
        normalized: dict[str, Any] = {str(key): value for key, value in binding.items()}
        if "contract" in normalized and "evidence_contract" not in normalized:
            normalized["evidence_contract"] = normalized["contract"]
        if any(normalized.get(key) != value for key, value in identity.items()):
            raise ValueError(f"{role} companion anchor binding mismatch")
        if role == "fault_runtime_matrix":
            production = result.get("production_build_identity_sha256")
            if production != anchor.get("build_identity_sha256"):
                raise ValueError("runtime production build identity mismatch")
        else:
            production = normalized.get("build_identity_sha256")
            if production != anchor.get("build_identity_sha256"):
                raise ValueError(f"{role} production build identity mismatch")


def _validate_formal_campaign_graph(*, campaign_id: str,
                                    registrations: Mapping[str, Mapping[str, Any]],
                                    retained_source: Mapping[str, Any],
                                    object_store: Path) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    """Validate the v2 five-object closure shared by finalization and validation."""
    if set(registrations) != set(_FINAL_ROLE_ORDER):
        raise ValueError("campaign registration roles must be exact")
    formal = dict(registrations["exact_anchor"])
    source = dict(retained_source)
    if formal.get("evidence_contract", {}).get("id") != "mw_perf.p0a_contract.v9":
        raise ValueError("formal registration must use v9")
    if source.get("role") != "exact_anchor" or source.get("evidence_contract", {}).get("id") != "mw_perf.p0a_contract.v8":
        raise ValueError("retained source must be the v8 exact anchor")
    all_descriptors = [formal, source] + [dict(registrations[role]) for role in _FINAL_ROLE_ORDER[1:]]
    if any(item.get("campaign_id") != campaign_id for item in all_descriptors):
        raise ValueError("campaign identity mismatch")
    for field in ("object_sha256", "companion_id"):
        values = [item.get(field) for item in all_descriptors]
        if any(not isinstance(value, str) for value in values) or len(set(values)) != 5:
            raise ValueError(f"duplicate descriptor {field}")

    source_path = _checked_object_path(Path(object_store), source["object_sha256"])
    source_generic = validate_companion_object(source_path, source)
    if source_generic.get("valid") is not True:
        raise ValueError("retained v8 source validation failed")
    source_result = validate_exact_anchor_companion(source_path, source)
    if source_result.get("valid") is not True:
        raise ValueError("retained v8 source specialized validation failed")
    results: dict[str, dict[str, Any]] = {"exact_anchor": source_result}
    checks: list[dict[str, Any]] = []
    formal_path = _checked_object_path(Path(object_store), formal["object_sha256"])
    formal_generic = validate_companion_object(formal_path, formal)
    if formal_generic.get("valid") is not True:
        raise ValueError("formal companion validation failed")
    from .formal_anchor import validate_formal_exact_anchor_companion
    formal_result = validate_formal_exact_anchor_companion(
        formal_path, formal, object_store=Path(object_store),
        expected_source_descriptor=source,
        expected_companion_descriptors={
            role: dict(registrations[role]) for role in _FINAL_ROLE_ORDER[1:]})
    if formal_result.get("valid") is not True:
        raise ValueError("formal exact anchor validation failed")
    if (formal.get("run_id") != source.get("run_id")
            or formal.get("build_identity_sha256") != source.get("build_identity_sha256")):
        raise ValueError("formal outer run/build binding mismatch")
    checks.append({"role": "exact_anchor", "object_sha256": formal["object_sha256"],
                   "generic_valid": True, "specialized_valid": True,
                   "check_type": "registration"})
    for role in _FINAL_ROLE_ORDER[1:]:
        descriptor = dict(registrations[role])
        path = _checked_object_path(Path(object_store), descriptor["object_sha256"])
        generic = validate_companion_object(path, descriptor)
        if generic.get("valid") is not True:
            raise ValueError(f"companion validation failed for {role}")
        result = _specialized_companion(role, path, descriptor)
        if result.get("valid") is not True:
            raise ValueError(f"{role} companion validation failed")
        results[role] = result
        checks.append({"role": role, "object_sha256": descriptor["object_sha256"],
                       "generic_valid": True, "specialized_valid": True,
                       "check_type": "registration"})
    _validate_role_bindings(results)
    checks.append({"role": "exact_anchor", "object_sha256": source["object_sha256"],
                   "generic_valid": True, "specialized_valid": True,
                   "check_type": "retained_v8_source"})
    return results, checks


def _publish_campaign_documents(root: Path, documents: Mapping[str, bytes]) -> None:
    staging: Path | None = None
    try:
        for _ in range(128):
            candidate = root.parent / f".{root.name}.{secrets.token_hex(12)}.staging"
            try:
                candidate.mkdir(mode=0o700)
                staging = candidate
                break
            except FileExistsError:
                continue
        if staging is None:
            raise OSError("unable to allocate campaign staging directory")
        for name, data in documents.items():
            _write_campaign_file(staging, name, data)
        stage_fd = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                           | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(stage_fd)
        finally:
            os.close(stage_fd)
        _rename_noreplace(staging, root)
        staging = None
        parent_fd = os.open(root.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                            | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)


def finalize_formal_campaign(campaign_root: Path, *, campaign_id: str,
                             descriptors: Mapping[str, Mapping[str, Any]],
                             retained_v8_source_descriptor: Mapping[str, Any],
                             object_store: Path) -> dict[str, Any]:
    """Atomically publish the closed v2 formal campaign."""
    root = Path(campaign_root)
    if not _valid_id(campaign_id):
        raise ValueError("campaign_id must be a non-empty string")
    if root.exists():
        raise FileExistsError(f"campaign root already exists: {root}")
    if not root.parent.is_dir() or root.parent.is_symlink():
        raise ValueError("campaign parent must be a real directory")
    normalized = {role: dict(descriptors[role]) for role in descriptors} if isinstance(descriptors, Mapping) else {}
    results, checks = _validate_formal_campaign_graph(
        campaign_id=campaign_id, registrations=normalized,
        retained_source=retained_v8_source_descriptor, object_store=Path(object_store))
    registrations = [{"role": role, "descriptor": normalized[role]} for role in _FINAL_ROLE_ORDER]
    retained = dict(retained_v8_source_descriptor)
    classification = {"valid_for_p0a_completion": True, "metric_availability": "unsupported",
                      "valid_for_comparison": False}
    manifest = {"schema": "mw_perf.evidence_campaign.v2", "campaign_id": campaign_id,
                "classification": classification, "threat_model": "cooperative_contained_sut_no_active_same_uid",
                "registrations": registrations, "retained_v8_source_descriptor": retained}
    validation = {"schema": "mw_perf.evidence_campaign_validation.v2", "campaign_id": campaign_id,
                  "collector_authored": True, "valid": True, "classification": classification,
                  "checks": checks}
    registration_set = _hash(_REGISTRATION_SET_DOMAIN_V2 + _canonical_json_bytes(
        {"registrations": registrations, "retained_v8_source_descriptor": retained}))
    manifest_raw = _canonical_json_bytes(manifest, newline=True)
    validation_raw = _canonical_json_bytes(validation, newline=True)
    manifest_sha = _hash(manifest_raw); validation_sha = _hash(validation_raw)
    root_sha = _hash(_CAMPAIGN_ROOT_DOMAIN_V2 + _canonical_json_bytes({
        "campaign_id": campaign_id, "manifest_sha256": manifest_sha,
        "registration_set_sha256": registration_set, "validation_sha256": validation_sha}))
    seal = {"schema": "mw_perf.evidence_campaign_seal.v2", "campaign_id": campaign_id,
            "manifest_sha256": manifest_sha, "validation_sha256": validation_sha,
            "registration_set_sha256": registration_set, "campaign_root_sha256": root_sha,
            "finalized": True}
    _validate_campaign_document(manifest, "evidence-campaign.v2.schema.json")
    _validate_campaign_document(validation, "evidence-campaign-validation.v2.schema.json")
    _validate_campaign_document(seal, "evidence-campaign-seal.v2.schema.json")
    _publish_campaign_documents(root, {"campaign_manifest.json": manifest_raw,
                                       "campaign_validation.json": validation_raw,
                                       "campaign_seal.json": _canonical_json_bytes(seal, newline=True)})
    return {"manifest": manifest, "validation": validation, "seal": seal,
            "manifest_sha256": manifest_sha, "validation_sha256": validation_sha,
            "registration_set_sha256": registration_set, "campaign_root_sha256": root_sha}


def _write_campaign_file(directory: Path, name: str, data: bytes) -> None:
    flags = (os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
             | getattr(os, "O_NOFOLLOW", 0))
    fd = os.open(directory / name, flags, 0o600)
    try:
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            if written <= 0:
                raise OSError("campaign write made no progress")
            view = view[written:]
        os.fsync(fd)
    finally:
        os.close(fd)


def _rename_noreplace(source: Path, destination: Path) -> None:
    """Atomically publish a directory without replacing an existing name."""
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise OSError(errno.ENOSYS, "renameat2 is required for exclusive campaign publication")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                          ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(-100, os.fsencode(source), -100, os.fsencode(destination), 1)
    if result != 0:
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            raise FileExistsError(error, os.strerror(error), destination)
        raise OSError(error, os.strerror(error), destination)


def finalize_campaign(campaign_root: Path, *, campaign_id: str,
                      descriptors: Mapping[str, Mapping[str, Any]],
                      object_store: Path) -> dict[str, Any]:
    """Revalidate and atomically publish one closed four-object evidence campaign.

    Companion IDs and object hashes must each be unique. A fault companion may
    legitimately retain the exact anchor run ID it evaluates.
    """
    root = Path(campaign_root)
    if not _valid_id(campaign_id):
        raise ValueError("campaign_id must be a non-empty string")
    if not isinstance(descriptors, Mapping) or set(descriptors) != set(_FINAL_ROLE_ORDER):
        raise ValueError("campaign descriptor roles must be exact")
    try:
        root.lstat()
    except FileNotFoundError:
        pass
    else:
        raise FileExistsError(f"campaign root already exists: {root}")
    try:
        parent_info = root.parent.lstat()
    except OSError as exc:
        raise ValueError("campaign parent is unavailable") from exc
    if not stat.S_ISDIR(parent_info.st_mode):
        raise ValueError("campaign parent must be a real directory")

    normalized: dict[str, dict[str, Any]] = {}
    for role in _FINAL_ROLE_ORDER:
        descriptor = descriptors[role]
        if not isinstance(descriptor, Mapping):
            raise TypeError("campaign descriptors must be mappings")
        copied = dict(descriptor)
        if copied.get("role") != role:
            raise ValueError(f"descriptor role mismatch for {role}")
        if copied.get("campaign_id") != campaign_id:
            raise ValueError(f"descriptor campaign_id mismatch for {role}")
        normalized[role] = copied
    for field in ("companion_id", "object_sha256"):
        values = [normalized[role].get(field) for role in _FINAL_ROLE_ORDER]
        if len(set(values)) != len(values):
            raise ValueError(f"duplicate descriptor {field}")

    checks: list[dict[str, Any]] = []
    registrations: list[dict[str, Any]] = []
    specialized_results: dict[str, dict[str, Any]] = {}
    for role in _FINAL_ROLE_ORDER:
        descriptor = normalized[role]
        object_sha256 = descriptor.get("object_sha256")
        if not isinstance(object_sha256, str):
            raise ValueError(f"invalid object_sha256 for {role}")
        path = _checked_object_path(Path(object_store), object_sha256)
        generic = validate_companion_object(path, descriptor)
        if generic.get("valid") is not True:
            raise ValueError(f"companion validation failed for {role}")
        specialized_result = _specialized_companion(role, path, descriptor)
        specialized_results[role] = specialized_result
        specialized = specialized_result.get("valid") is True
        if not specialized:
            raise ValueError(f"{role} companion validation failed")
        if role == "exact_anchor":
            # The exact descriptor is caller-supplied and therefore provides its build identity.
            specialized_result.setdefault("build_identity_sha256", descriptor["build_identity_sha256"])
        registrations.append({"role": role, "descriptor": descriptor})
        checks.append({
            "role": role,
            "object_sha256": descriptor["object_sha256"],
            "generic_valid": True,
            "specialized_valid": specialized,
        })
    _validate_role_bindings(specialized_results)

    classification = {
        "classification": "candidate",
        "failure_code": "P0A_ARTIFACT",
        "metric_availability": "unsupported",
        "valid_for_comparison": False,
    }
    manifest = {
        "schema": "mw_perf.evidence_campaign.v1",
        "campaign_id": campaign_id,
        "classification": classification,
        "threat_model": "cooperative_contained_sut_no_active_same_uid",
        "registrations": registrations,
        "limitations": [
            "This campaign is candidate evidence only and provides no performance acceptance.",
            "This campaign is not formal P0a acceptance.",
        ],
    }
    validation = {
        "schema": "mw_perf.evidence_campaign_validation.v1",
        "campaign_id": campaign_id,
        "collector_authored": True,
        "valid": True,
        "classification": classification,
        "checks": checks,
    }
    registration_set_sha256 = _hash(
        _REGISTRATION_SET_DOMAIN + _canonical_json_bytes(registrations))
    manifest_bytes = _canonical_json_bytes(manifest, newline=True)
    validation_bytes = _canonical_json_bytes(validation, newline=True)
    manifest_sha256 = _hash(manifest_bytes)
    validation_sha256 = _hash(validation_bytes)
    root_sha256 = _hash(_CAMPAIGN_ROOT_DOMAIN + _canonical_json_bytes({
        "campaign_id": campaign_id,
        "manifest_sha256": manifest_sha256,
        "registration_set_sha256": registration_set_sha256,
        "validation_sha256": validation_sha256,
    }))
    seal = {
        "schema": "mw_perf.evidence_campaign_seal.v1",
        "campaign_id": campaign_id,
        "manifest_sha256": manifest_sha256,
        "validation_sha256": validation_sha256,
        "registration_set_sha256": registration_set_sha256,
        "campaign_root_sha256": root_sha256,
        "finalized": True,
    }
    seal_bytes = _canonical_json_bytes(seal, newline=True)
    seal_sha256 = _hash(seal_bytes)
    _validate_campaign_document(manifest, "evidence-campaign.schema.v1.json")
    _validate_campaign_document(validation, "evidence-campaign-validation.schema.v1.json")
    _validate_campaign_document(seal, "evidence-campaign-seal.schema.v1.json")

    staging: Path | None = None
    try:
        for _ in range(128):
            candidate = root.parent / f".{root.name}.{secrets.token_hex(12)}.staging"
            try:
                candidate.mkdir(mode=0o700)
                staging = candidate
                break
            except FileExistsError:
                continue
        if staging is None:
            raise OSError("unable to allocate campaign staging directory")
        _write_campaign_file(staging, "campaign_manifest.json", manifest_bytes)
        _write_campaign_file(staging, "campaign_validation.json", validation_bytes)
        _write_campaign_file(staging, "campaign_seal.json", seal_bytes)
        stage_fd = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                           | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(stage_fd)
        finally:
            os.close(stage_fd)
        _rename_noreplace(staging, root)
        staging = None
        parent_fd = os.open(root.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                            | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)

    return {
        "manifest": manifest,
        "validation": validation,
        "seal": seal,
        "manifest_sha256": manifest_sha256,
        "validation_sha256": validation_sha256,
        "seal_sha256": seal_sha256,
        "registration_set_sha256": registration_set_sha256,
        "campaign_root_sha256": root_sha256,
    }


def _read_campaign_json(root: Path, name: str) -> tuple[dict[str, Any], bytes]:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = -1
    try:
        fd = os.open(root / name, flags)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError(f"{name} is not a regular file")
        data = os.read(fd, info.st_size + 1)
        if len(data) != info.st_size:
            raise ValueError(f"{name} changed while being read")
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"{name} is unavailable") from exc
    finally:
        if fd >= 0:
            os.close(fd)
    value = strict_json_loads(data)
    if not isinstance(value, dict) or data != _canonical_json_bytes(value, newline=True):
        raise ValueError(f"{name} is not canonical JSON")
    return value, data


def _validate_formal_campaign_documents(*, manifest: dict[str, Any], manifest_raw: bytes,
                                        validation: dict[str, Any], validation_raw: bytes,
                                        seal: dict[str, Any], object_store: Path,
                                        expected_campaign_root_sha256: str) -> None:
    """Validate v2 documents and close their five immutable objects."""
    _validate_campaign_document(manifest, "evidence-campaign.v2.schema.json")
    _validate_campaign_document(validation, "evidence-campaign-validation.v2.schema.json")
    _validate_campaign_document(seal, "evidence-campaign-seal.v2.schema.json")
    registrations = manifest["registrations"]
    if [item["role"] for item in registrations] != list(_FINAL_ROLE_ORDER):
        raise ValueError("campaign registration order is invalid")
    if validation["campaign_id"] != manifest["campaign_id"] or seal["campaign_id"] != manifest["campaign_id"]:
        raise ValueError("campaign identifiers do not match")
    retained = manifest["retained_v8_source_descriptor"]
    registration_set = _hash(_REGISTRATION_SET_DOMAIN_V2 + _canonical_json_bytes(
        {"registrations": registrations, "retained_v8_source_descriptor": retained}))
    manifest_sha = _hash(manifest_raw); validation_sha = _hash(validation_raw)
    root_sha = _hash(_CAMPAIGN_ROOT_DOMAIN_V2 + _canonical_json_bytes({
        "campaign_id": manifest["campaign_id"], "manifest_sha256": manifest_sha,
        "registration_set_sha256": registration_set, "validation_sha256": validation_sha}))
    if (seal["manifest_sha256"] != manifest_sha or seal["validation_sha256"] != validation_sha
            or seal["registration_set_sha256"] != registration_set
            or seal["campaign_root_sha256"] != root_sha or seal["finalized"] is not True
            or root_sha != expected_campaign_root_sha256):
        raise ValueError("campaign seal or external root is invalid")
    descriptors = {item["role"]: item["descriptor"] for item in registrations}
    _results, checks = _validate_formal_campaign_graph(
        campaign_id=manifest["campaign_id"], registrations=descriptors,
        retained_source=retained, object_store=Path(object_store))
    if validation["checks"] != checks:
        raise ValueError("campaign checks are invalid")


def validate_campaign(campaign_root: Path, *, object_store: Path,
                      expected_campaign_root_sha256: str) -> dict[str, Any]:
    """Verify a published campaign against an externally supplied root.

    The seal's self-reported root is only an integrity field; it is not a trust
    root without ``expected_campaign_root_sha256`` from an external channel.
    """
    codes: set[str] = set()
    manifest = validation = seal = None
    try:
        if not _valid_hash(expected_campaign_root_sha256):
            raise ValueError("external campaign root is required")
        root = Path(campaign_root)
        if not root.is_dir() or root.is_symlink():
            raise ValueError("campaign root is not a real directory")
        names = {item.name for item in root.iterdir()}
        if names != {"campaign_manifest.json", "campaign_validation.json", "campaign_seal.json"}:
            raise ValueError("campaign file set is not exact")
        manifest, manifest_raw = _read_campaign_json(root, "campaign_manifest.json")
        validation, validation_raw = _read_campaign_json(root, "campaign_validation.json")
        seal, seal_raw = _read_campaign_json(root, "campaign_seal.json")
        schema_pair = (manifest.get("schema"), validation.get("schema"), seal.get("schema"))
        if schema_pair == ("mw_perf.evidence_campaign.v2",
                           "mw_perf.evidence_campaign_validation.v2",
                           "mw_perf.evidence_campaign_seal.v2"):
            _validate_formal_campaign_documents(
                manifest=manifest, manifest_raw=manifest_raw,
                validation=validation, validation_raw=validation_raw, seal=seal,
                object_store=Path(object_store),
                expected_campaign_root_sha256=expected_campaign_root_sha256)
            return {"valid": True, "failure_codes": [], "manifest": manifest,
                    "validation": validation, "seal": seal}
        _validate_campaign_document(manifest, "evidence-campaign.schema.v1.json")
        _validate_campaign_document(validation, "evidence-campaign-validation.schema.v1.json")
        _validate_campaign_document(seal, "evidence-campaign-seal.schema.v1.json")
        registrations = manifest["registrations"]
        if ([item["role"] for item in registrations] != list(_FINAL_ROLE_ORDER)
                or len({item["descriptor"]["object_sha256"] for item in registrations}) != 4):
            raise ValueError("campaign registration order is invalid")
        for registration in registrations:
            if registration["role"] != registration["descriptor"].get("role"):
                raise ValueError("registration role does not match descriptor role")
        if validation["campaign_id"] != manifest["campaign_id"] or seal["campaign_id"] != manifest["campaign_id"]:
            raise ValueError("campaign identifiers do not match")
        registration_set = _hash(_REGISTRATION_SET_DOMAIN + _canonical_json_bytes(registrations))
        manifest_sha = _hash(manifest_raw)
        validation_sha = _hash(validation_raw)
        root_sha = _hash(_CAMPAIGN_ROOT_DOMAIN + _canonical_json_bytes({
            "campaign_id": manifest["campaign_id"], "manifest_sha256": manifest_sha,
            "registration_set_sha256": registration_set, "validation_sha256": validation_sha,
        }))
        if (seal["manifest_sha256"] != manifest_sha or seal["validation_sha256"] != validation_sha
                or seal["registration_set_sha256"] != registration_set
                or seal["campaign_root_sha256"] != root_sha or seal["finalized"] is not True
                or root_sha != expected_campaign_root_sha256):
            raise ValueError("campaign seal or external root is invalid")
        descriptors = {item["role"]: item["descriptor"] for item in registrations}
        results: dict[str, dict[str, Any]] = {}
        for role in _FINAL_ROLE_ORDER:
            descriptor = descriptors[role]
            path = _checked_object_path(Path(object_store), descriptor["object_sha256"])
            generic = validate_companion_object(path, descriptor)
            if generic.get("valid") is not True:
                raise ValueError(f"companion validation failed for {role}")
            results[role] = _specialized_companion(role, path, descriptor)
            if results[role].get("valid") is not True:
                raise ValueError(f"{role} companion validation failed")
        _validate_role_bindings(results)
        if ([item["role"] for item in validation["checks"]]
                != list(_FINAL_ROLE_ORDER)
                or any(item["object_sha256"] != descriptors[item["role"]]["object_sha256"]
                       for item in validation["checks"])):
            raise ValueError("campaign checks are invalid")
        return {"valid": True, "failure_codes": [], "manifest": manifest,
                "validation": validation, "seal": seal}
    except (OSError, ValueError, TypeError, KeyError, UnicodeError, json.JSONDecodeError):
        codes.add("CAMPAIGN_INVALID")
        return {"valid": False, "failure_codes": sorted(codes), "manifest": manifest,
                "validation": validation, "seal": seal}
