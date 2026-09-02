"""External, read-only IBMW P0 discovery adapter.

The adapter deliberately has no dependency on IBMW runtime internals.  Its small
public interface is ``collect_ibmw(root, **options)`` and returns the common
``manifest/topology/capability`` envelope expected by the collector.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
from pathlib import Path
from typing import Any

from ..json_strict import loads as strict_json_loads


def _read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (OSError, UnicodeError):
        return None


def _version(root: Path) -> dict[str, Any]:
    candidates = [root / "VERSION", root / "version.txt", root / "VERSION.txt"]
    for path in candidates:
        text = _read(path)
        if text:
            value = next((x for x in text.split() if re.fullmatch(r"v?\d+\.\d+(?:\.\d+)?(?:[-+].*)?", x)), text.strip())
            return {"value": value.lstrip("v"), "source": str(path)}
    header = _read(root / "src/api/cpp/util/version.h")
    if header:
        match = re.search(r"#define\s+IBMW_RUNTIME_VERSION\s+\"([^\"]+)\"", header)
        if match:
            return {"value": match.group(1), "source": "src/api/cpp/util/version.h"}
    try:
        commit = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True, timeout=2,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        commit = None
    return {"value": None, "source": None, "git_commit": commit}


def _cmake(root: Path, build_dir: str | Path | None, cmake_executable: str | None) -> dict[str, Any]:
    config = Path(build_dir).expanduser().resolve() if build_dir else None
    cache = config / "CMakeCache.txt" if config else None
    configured = bool(cache and cache.is_file())
    executable = cmake_executable
    if executable is None:
        executable = "cmake"
        try:
            result = subprocess.run([executable, "--version"], capture_output=True, text=True, timeout=2)
            available = result.returncode == 0
            version = result.stdout.splitlines()[0] if available and result.stdout else None
        except (OSError, subprocess.SubprocessError):
            available, version = False, None
    else:
        available, version = True, None
    options: dict[str, str] = {}
    if configured and cache is not None:
        for line in (_read(cache) or "").splitlines():
            if "=" in line and not line.startswith(("//", "#")):
                key, value = line.split("=", 1)
                if ":INTERNAL" not in key and ":STATIC" not in key:
                    options[key] = value
    return {"available": available, "executable": executable, "version": version,
            "configured": configured, "build_dir": str(config) if config else None,
            "options": options, "source": "CMakeCache.txt" if configured else "cmake --version",
            "cache_sha256": hashlib.sha256(cache.read_bytes()).hexdigest() if configured and cache else None}


def _digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _stable_stat_key(info: os.stat_result) -> tuple[int, int, int, int, int, int]:
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _assert_path_identity(source: Path, resolved: Path, expected: os.stat_result) -> None:
    try:
        current_resolved = source.resolve(strict=True)
        source_info = source.stat()
        resolved_info = resolved.stat()
    except OSError as exc:
        raise OSError(f"artifact path changed while hashing: {source}") from exc
    expected_identity = (expected.st_dev, expected.st_ino)
    if (current_resolved != resolved
            or (source_info.st_dev, source_info.st_ino) != expected_identity
            or (resolved_info.st_dev, resolved_info.st_ino) != expected_identity):
        raise OSError(f"artifact path changed while hashing: {source}")


def binary_identity(path: str | Path) -> dict[str, Any]:
    source = Path(path).expanduser()
    resolved = source.resolve(strict=True)
    try:
        fd = os.open(resolved, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
    except OSError as exc:
        raise ValueError(f"artifact is not a readable file: {source}") from exc
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ValueError(f"artifact is not a readable file: {source}")
        _assert_path_identity(source, resolved, before)
        digest = hashlib.sha256()
        while chunk := os.read(fd, 1024 * 1024):
            digest.update(chunk)
        after = os.fstat(fd)
        if _stable_stat_key(before) != _stable_stat_key(after):
            raise OSError(f"artifact changed while hashing: {source}")
        _assert_path_identity(source, resolved, after)
        return {"declared_path": str(source), "resolved_path": str(resolved),
                "device": before.st_dev, "inode": before.st_ino,
                "size": before.st_size, "mtime_ns": before.st_mtime_ns,
                "sha256": digest.hexdigest()}
    finally:
        os.close(fd)


def source_identity(root: str | Path) -> dict[str, Any]:
    source = Path(root).expanduser().resolve()
    def git(*args: str) -> bytes:
        return subprocess.run(["git", "-C", str(source), *args], check=True,
                              capture_output=True, timeout=15).stdout
    commit = git("rev-parse", "HEAD").decode().strip()
    branch = git("branch", "--show-current").decode().strip()
    diff = git("diff", "--binary", "HEAD", "--")
    untracked = git("ls-files", "--others", "--exclude-standard", "-z")
    # Local collection/build evidence is deliberately outside source identity,
    # even when an older checkout has not yet added it to .gitignore.
    excluded = (".opencode/", ".omc/", "build/", "runs/", "out/", "bench_results/")
    entries: list[bytes] = []
    count = 0
    for raw_name in untracked.split(b"\0"):
        if not raw_name:
            continue
        name = raw_name.decode("utf-8", "surrogateescape")
        normalized = name.replace(os.sep, "/")
        if normalized in {x.rstrip("/") for x in excluded} or normalized.startswith(excluded):
            continue
        path = source / name
        info = path.lstat()
        mode = stat.S_IMODE(info.st_mode)
        if stat.S_ISLNK(info.st_mode):
            kind, payload = "symlink", os.readlink(path).encode("utf-8", "surrogateescape")
        elif stat.S_ISREG(info.st_mode):
            kind, payload = "file", _digest(path.read_bytes()).encode()
        else:
            kind, payload = "other", b""
        entries.append(normalized.encode("utf-8", "surrogateescape") + b"\0" +
                       kind.encode() + b"\0" + format(mode, "o").encode() + b"\0" + payload)
        count += 1
    untracked_tree = _digest(b"untracked-tree-v1\0" + b"\0".join(sorted(entries)))
    diff_hash = _digest(b"git-diff-v1\0" + diff)
    working_tree = _digest(b"working-tree-v1\0" + bytes.fromhex(diff_hash) + bytes.fromhex(untracked_tree))
    return {"root": str(source), "commit": commit, "branch": branch,
            "dirty": bool(diff or count), "diff_sha256": diff_hash,
            "untracked_tree_sha256": untracked_tree, "untracked_count": count,
            "working_tree_sha256": working_tree}


def collect_provenance(root: str | Path, build: dict[str, Any], *, required: bool,
                       sut_executable: str | Path | None,
                       artifact_binaries: list[str | Path] | None) -> dict[str, Any]:
    artifacts = list(artifact_binaries or [])
    declared = list(artifacts)
    if sut_executable is not None: declared.insert(0, sut_executable)
    if required and (not sut_executable or not artifacts):
        raise ValueError("required IBMW provenance needs sut_executable and artifact_binaries")
    binaries = [binary_identity(path) for path in dict.fromkeys(map(str, declared))]
    if required and not build.get("configured"):
        raise ValueError("required IBMW provenance needs a configured build_dir/CMakeCache.txt")
    return {"required": required, "binaries": binaries,
            "source": source_identity(root) if required or binaries else None, "build": build}


def _transport(path: str | Path | None) -> dict[str, Any]:
    if not path:
        return {"configured": False, "config_path": None, "summary": {}}
    config_path = Path(path)
    text = _read(config_path)
    if text is None:
        return {"configured": False, "config_path": str(config_path), "summary": {}, "error": "unreadable"}
    summary: dict[str, Any] = {}
    # Keep this dependency-free and intentionally summarize only transport-like keys.
    for line in text.splitlines():
        match = re.match(r"\s*([A-Za-z0-9_.-]*(?:transport|driver|shm|dds|iceoryx|zenoh|mqtt)[A-Za-z0-9_.-]*)\s*:\s*(.*?)\s*(?:#.*)?$", line, re.I)
        if match:
            value: Any = match.group(2).strip().strip("'\"")
            if value.lower() in {"true", "false"}:
                value = value.lower() == "true"
            summary[match.group(1)] = value
    return {"configured": True, "config_path": str(config_path), "summary": summary}


def _core_capability(path: str | Path | None) -> dict[str, Any]:
    if not path:
        return {"status": "unsupported", "available": False, "reason": "core capability file not supplied"}
    capability_path = Path(path)
    text = _read(capability_path)
    if text is None:
        return {"status": "unsupported", "available": False, "path": str(capability_path), "reason": "file unavailable"}
    try:
        value = strict_json_loads(text)
    except (json.JSONDecodeError, ValueError):
        value = {"raw": text.strip()}
    return {"status": "supported", "available": True, "path": str(capability_path), "value": value}


def core_metrics_config(value: Any) -> dict[str, Any]:
    """Normalize the optional JSONL configuration (a path string is shorthand)."""
    if value is None:
        return {"configured": False, "required": False, "path": None}
    if isinstance(value, (str, Path)):
        return {"configured": True, "required": False, "path": str(value)}
    if not isinstance(value, dict):
        raise TypeError("ibmw.core_metrics must be a path or object")
    path = value.get("path", value.get("directory"))
    if not isinstance(path, (str, Path)) or not str(path):
        raise ValueError("ibmw.core_metrics.path (or directory) is required")
    return {"configured": True, "required": value.get("required", False) is True, "path": str(path)}


def resolve_core_metrics(value: Any) -> Path | None:
    config = core_metrics_config(value)
    if not config["configured"]:
        return None
    path = Path(config["path"]).expanduser()
    return path / "metrics.jsonl" if path.is_dir() or path.name != "metrics.jsonl" and not path.suffix else path


def core_metrics_snapshot(value: Any) -> dict[str, Any]:
    path = resolve_core_metrics(value)
    if path is None:
        return {"exists": False, "path": None}
    try:
        stat = path.stat()
        content_sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        return {"exists": path.is_file(), "path": str(path), "mtime_ns": stat.st_mtime_ns,
                "size": stat.st_size, "inode": stat.st_ino, "content_sha256": content_sha256}
    except OSError:
        return {"exists": False, "path": str(path)}


def validate_core_metrics(path: Path) -> dict[str, Any]:
    errors: list[str] = []
    kinds: set[str] = set()
    count = 0
    previous = 0
    try:
        data = path.read_bytes()
    except OSError as exc:
        return {"valid": False, "record_count": 0, "kinds": [], "validation_errors": [f"unreadable: {exc}"]}
    if not data:
        errors.append("file is empty")
    if data and not data.endswith(b"\n"):
        errors.append("last line is incomplete (missing newline)")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        return {"valid": False, "record_count": 0, "kinds": [], "validation_errors": [f"invalid UTF-8: {exc}"]}
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            errors.append(f"line {number}: empty line")
            continue
        try:
            record = strict_json_loads(line)
        except (json.JSONDecodeError, ValueError) as exc:
            errors.append(f"line {number}: invalid JSON: {exc}")
            continue
        if not isinstance(record, dict):
            errors.append(f"line {number}: record must be an object")
            continue
        count += 1
        if record.get("schema") != "ibmw.core_metrics.v1":
            errors.append(f"line {number}: invalid schema")
        seq = record.get("snapshot_seq")
        if not isinstance(seq, int) or isinstance(seq, bool) or seq <= 0:
            errors.append(f"line {number}: snapshot_seq must be a positive integer")
        elif seq <= previous:
            errors.append(f"line {number}: snapshot_seq must be strictly increasing")
        else:
            previous = seq
        kind = record.get("kind")
        if not isinstance(kind, str) or not kind:
            errors.append(f"line {number}: kind must be a non-empty string")
        else:
            kinds.add(kind)
        if not isinstance(record.get("cumulative"), dict):
            errors.append(f"line {number}: cumulative must be an object")
        if record.get("complete") is not True:
            errors.append(f"line {number}: complete must be true")
    return {"valid": not errors, "record_count": count, "kinds": sorted(kinds), "validation_errors": errors}


def archive_core_metrics(value: Any, baseline: dict[str, Any], destination: Path, *, managed: bool,
                          graceful_finalization: bool = True, run_id: str | None = None,
                          adapter: str = "ibmw", build: Any = None, root: str | None = None,
                          mode: str | None = None, sut_pid: int | None = None,
                          sut_start_time_ticks: int | None = None) -> dict[str, Any]:
    config = core_metrics_config(value)
    metadata = ("exists", "path", "mtime_ns", "size", "inode")
    current = core_metrics_snapshot(value)
    source = resolve_core_metrics(value)
    source_path = str(source) if source is not None else None
    effective_mode = mode or ("managed" if managed else "attached")
    final_snapshot = bool(managed and effective_mode == "managed" and graceful_finalization and current.get("exists"))
    provenance = {
        "run_id": run_id, "adapter": adapter, "build": build, "root": root,
        "mode": effective_mode, "sut_pid": sut_pid, "sut_start_time_ticks": sut_start_time_ticks,
        "source_path": source_path,
        "source_sha256": current.get("content_sha256"), "archive_sha256": None,
        "baseline_metadata": {key: baseline.get(key) for key in metadata},
        "final_metadata": {key: current.get(key) for key in metadata},
        "graceful_finalization": bool(graceful_finalization and effective_mode == "managed"),
        "final_snapshot": final_snapshot,
    }
    result: dict[str, Any] = {"required": config["required"], "archive_path": None,
                              "record_count": 0, "kinds": [], "validation_errors": [],
                              "provenance": provenance}
    if source is None:
        return {**result, "status": "unsupported", "reason": "core metrics not configured"}
    if not current["exists"]:
        reason = "no final artifact after managed SUT stop" if managed else "attached mode has no final artifact; SUT was not stopped"
        return {**result, "status": "unsupported", "source_path": str(source), "reason": reason}
    diagnostics = {"baseline": {key: baseline.get(key) for key in metadata},
                   "final": {key: current.get(key) for key in metadata}}
    if (baseline.get("exists") and baseline.get("content_sha256") is not None
            and current.get("content_sha256") == baseline.get("content_sha256")
            and current.get("inode") == baseline.get("inode")):
        return {**result, "status": "stale", "source_path": str(source),
                "content_sha256": current.get("content_sha256"), "metadata_diagnostics": diagnostics,
                "reason": "artifact content unchanged since run baseline"}
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(source, destination)
    except OSError as exc:
        return {**result, "status": "invalid", "source_path": str(source), "validation_errors": [f"archive failed: {exc}"]}
    validation = validate_core_metrics(destination)
    try:
        provenance["archive_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    except OSError:
        provenance["archive_sha256"] = None
    controlled = effective_mode == "managed" and managed and graceful_finalization and final_snapshot
    if validation["valid"] and controlled:
        status, reason = "supported", None
    elif validation["valid"] and effective_mode == "attached":
        status = "invalid" if config["required"] else "partial"
        reason = "attached mode has no controlled finalization; archived artifact is diagnostic only"
    else:
        status = "invalid"
        reason = "managed SUT finalization was not graceful; final snapshot not confirmed"
    return {**result, **validation, "status": status, "source_path": str(source),
            "archive_path": str(destination), "content_sha256": current.get("content_sha256"),
            "metadata_diagnostics": diagnostics, **({"reason": reason} if reason else {})}


def collect_ibmw(
    root: str | Path,
    *,
    build_dir: str | Path | None = None,
    cmake_executable: str | None = None,
    transport_config: str | Path | None = None,
    core_capability: str | Path | None = None,
    core_metrics: Any = None,
    provenance: Any = None,
    sut_executable: str | Path | None = None,
    artifact_binaries: list[str | Path] | None = None,
) -> dict[str, Any]:
    """Collect a deterministic external IBMW manifest without changing the source tree."""
    source = Path(root).expanduser().resolve()
    cmake = _cmake(source, build_dir, cmake_executable)
    core = _core_capability(core_capability)
    metrics_config = core_metrics_config(core_metrics)
    provenance_config = provenance if isinstance(provenance, dict) else {}
    provenance_required = provenance is True or provenance_config.get("required") is True
    sut_executable = provenance_config.get("sut_executable", sut_executable)
    artifact_binaries = provenance_config.get("artifact_binaries", artifact_binaries)
    provenance_result = collect_provenance(source, cmake, required=provenance_required,
                                           sut_executable=sut_executable,
                                           artifact_binaries=artifact_binaries)
    if metrics_config["configured"]:
        core = {"status": "unsupported", "available": False, "required": metrics_config["required"],
                "source_path": str(resolve_core_metrics(core_metrics)), "reason": "awaiting final artifact"}
    return {
        "schema_version": "mw_perf_collect.v1",
        "manifest": {"middleware": "ibmw", "source_root": str(source), "version": _version(source), "build": cmake,
                     "provenance": provenance_result},
        "topology": {"transport": _transport(transport_config), "runtime": {"manifest": "external-only", "drivers": []}},
        "capability": {"core_metrics": core, "external_p0": {"status": "supported", "features": ["version", "cmake", "transport_summary"]},
                       "degradations": [] if core["available"] else ["core_metrics: unsupported; external P0 collection continues"]},
    }


__all__ = ["archive_core_metrics", "binary_identity", "collect_ibmw", "collect_provenance",
           "core_metrics_config", "core_metrics_snapshot", "source_identity", "validate_core_metrics"]
