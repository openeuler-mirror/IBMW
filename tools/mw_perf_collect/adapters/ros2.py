"""Passive ROS 2 P0 collection adapter.

The adapter deliberately uses ROS 2 CLI subprocesses instead of creating an
observer or importing rclpy.  This keeps collection usable when ROS is not
installed and avoids changing DDS matching behaviour.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import subprocess
import time
import re
import struct
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from ..json_strict import loads as strict_json_loads


CommandRunner = Callable[..., Any]

LATENCY_SCHEMA = "mw_perf.latency.v1"
LATENCY_COMPLETION_KIND = "completion"
LATENCY_CLOCK_DOMAIN = "producer_monotonic"


def latency_artifact_config(value: Any) -> dict[str, Any]:
    """Normalize the producer-owned latency artifact configuration."""
    if value is None:
        return {"configured": False, "required": False, "path": None}
    if isinstance(value, (str, Path)):
        return {"configured": True, "required": False, "path": str(value)}
    if not isinstance(value, Mapping):
        raise TypeError("ros2.latency_artifact must be a path or object")
    path = value.get("path", value.get("directory"))
    if not isinstance(path, (str, Path)) or not str(path):
        raise ValueError("ros2.latency_artifact.path (or directory) is required")
    return {"configured": True, "required": value.get("required") is True, "path": str(path)}


def resolve_latency_artifact(value: Any) -> Path | None:
    config = latency_artifact_config(value)
    if not config["configured"]:
        return None
    path = Path(config["path"]).expanduser()
    return path / "latency.jsonl" if path.is_dir() or (not path.suffix and path.name != "latency.jsonl") else path


def latency_artifact_snapshot(value: Any) -> dict[str, Any]:
    path = resolve_latency_artifact(value)
    if path is None:
        return {"exists": False, "path": None}
    try:
        stat = path.stat()
        return {"exists": path.is_file(), "path": str(path), "mtime_ns": stat.st_mtime_ns,
                "size": stat.st_size, "inode": stat.st_ino,
                "content_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    except OSError:
        return {"exists": False, "path": str(path)}


def validate_latency_artifact(path: Path, *, expected_run_id: str | None = None,
                              expected_producer_pid: int | None = None,
                              expected_producer_start_time_ticks: int | None = None) -> dict[str, Any]:
    """Validate the strict, single-topic/type producer JSONL contract."""
    errors: list[str] = []
    count = 0
    previous: int | None = None
    topic: str | None = None
    message_type: str | None = None
    completion: dict[str, Any] | None = None
    clock_domain: str | None = None
    legacy = False
    try:
        data = path.read_bytes()
    except OSError as exc:
        return {"valid": False, "record_count": 0, "topic": None, "type": None,
                "validation_errors": [f"unreadable: {exc}"]}
    if not data:
        errors.append("file is empty")
    if data and not data.endswith(b"\n"):
        errors.append("last line is incomplete (missing newline)")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        return {"valid": False, "record_count": 0, "topic": None, "type": None,
                "validation_errors": [f"invalid UTF-8: {exc}"]}
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
        if record.get("kind") == LATENCY_COMPLETION_KIND:
            if number != len(text.splitlines()):
                errors.append(f"line {number}: completion must be the final record")
            if completion is not None:
                errors.append(f"line {number}: duplicate completion")
            completion = record
            current_required = {"schema", "kind", "run_id", "producer_pid", "producer_start_time_ticks",
                                "record_count", "topic", "type", "complete", "clock_domain"}
            if set(record) == current_required:
                clock_domain = record.get("clock_domain")
                if clock_domain != LATENCY_CLOCK_DOMAIN:
                    errors.append(f"line {number}: unsupported clock_domain")
            elif set(record) == current_required - {"clock_domain"}:
                legacy = True
            else:
                errors.append(f"line {number}: completion fields must exactly match the contract")
            continue
        count += 1
        required_data = {"schema", "seq", "send_ns", "receive_ns", "latency_ns", "topic",
                         "type", "payload_bytes", "valid"}
        if set(record) != required_data:
            errors.append(f"line {number}: data fields must exactly match the contract")
        if record.get("schema") != LATENCY_SCHEMA:
            errors.append(f"line {number}: invalid schema")
        seq = record.get("seq")
        if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
            errors.append(f"line {number}: seq must be a non-negative integer")
        elif previous is not None and seq <= previous:
            errors.append(f"line {number}: seq must be strictly increasing")
        else:
            previous = seq
        send, receive, latency = record.get("send_ns"), record.get("receive_ns"), record.get("latency_ns")
        times_valid = (isinstance(send, int) and not isinstance(send, bool) and send > 0
                       and isinstance(receive, int) and not isinstance(receive, bool) and receive > 0)
        if not times_valid:
            errors.append(f"line {number}: send_ns and receive_ns must be positive integers")
        else:
            assert isinstance(send, int) and isinstance(receive, int)
            send_value, receive_value = send, receive
            if not isinstance(latency, int) or isinstance(latency, bool) or latency < 0 or latency != receive_value - send_value:
                errors.append(f"line {number}: latency_ns must be non-negative and equal receive_ns-send_ns")
        current_topic, current_type = record.get("topic"), record.get("type")
        if not isinstance(current_topic, str) or not current_topic:
            errors.append(f"line {number}: topic must be a non-empty string")
        elif topic is None: topic = current_topic
        elif current_topic != topic: errors.append(f"line {number}: artifact must contain exactly one topic")
        if not isinstance(current_type, str) or not current_type:
            errors.append(f"line {number}: type must be a non-empty string")
        elif message_type is None: message_type = current_type
        elif current_type != message_type: errors.append(f"line {number}: artifact must contain exactly one type")
        payload = record.get("payload_bytes")
        if not isinstance(payload, int) or isinstance(payload, bool) or payload < 0:
            errors.append(f"line {number}: payload_bytes must be a non-negative integer")
        if record.get("valid") is not True:
            errors.append(f"line {number}: valid must be true")
    if count == 0:
        errors.append("artifact contains no data records")
    if completion is None:
        errors.append("completion record is missing")
    else:
        if completion.get("schema") != LATENCY_SCHEMA or completion.get("complete") is not True:
            errors.append("completion schema/complete is invalid")
        if completion.get("record_count") != count:
            errors.append("completion record_count does not match data records")
        if not isinstance(completion.get("topic"), str) or not completion.get("topic"):
            errors.append("completion topic must be a non-empty string")
        if not isinstance(completion.get("type"), str) or not completion.get("type"):
            errors.append("completion type must be a non-empty string")
        if completion.get("topic") != topic or completion.get("type") != message_type:
            errors.append("completion topic/type does not match data records")
        for field in ("producer_pid", "producer_start_time_ticks"):
            value = completion.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                errors.append(f"completion {field} must be a positive integer")
        if not isinstance(completion.get("run_id"), str) or not completion.get("run_id"):
            errors.append("completion run_id must be a non-empty string")
        expected = (("run_id", expected_run_id), ("producer_pid", expected_producer_pid),
                    ("producer_start_time_ticks", expected_producer_start_time_ticks))
        for field, value in expected:
            if value is not None and completion.get(field) != value:
                errors.append(f"completion {field} does not match expected run identity")
    return {"valid": not errors, "record_count": count, "topic": topic, "type": message_type,
            "completion": completion, "clock_domain": clock_domain, "legacy": legacy,
            "validation_errors": errors}


def archive_latency_artifact(value: Any, baseline: dict[str, Any], destination: Path, *, managed: bool,
                             graceful_finalization: bool, run_id: str, mode: str, sut_pid: int,
                             sut_start_time_ticks: int) -> dict[str, Any]:
    config = latency_artifact_config(value)
    current = latency_artifact_snapshot(value)
    source = resolve_latency_artifact(value)
    metadata = ("exists", "path", "mtime_ns", "size", "inode")
    provenance = {"run_id": run_id, "adapter": "ros2", "mode": mode, "sut_pid": sut_pid,
        "sut_start_time_ticks": sut_start_time_ticks, "source_path": str(source) if source else None,
        "source_sha256": current.get("content_sha256"), "archive_sha256": None,
        "baseline_metadata": {key: baseline.get(key) for key in metadata},
        "final_metadata": {key: current.get(key) for key in metadata},
        "graceful_finalization": bool(graceful_finalization and mode == "managed"),
        "producer_completion_verified": False, "clock_domain": None}
    result = {"required": config["required"], "archive_path": None, "record_count": 0,
              "topic": None, "type": None, "validation_errors": [], "provenance": provenance}
    if source is None:
        return {**result, "status": "invalid" if config["required"] else "unsupported",
                "reason": "latency artifact not configured"}
    if not current.get("exists"):
        return {**result, "status": "invalid" if config["required"] else "unsupported",
                "source_path": str(source), "reason": "producer artifact unavailable"}
    if baseline.get("exists") and baseline.get("content_sha256") == current.get("content_sha256"):
        return {**result, "status": "invalid" if config["required"] else "stale", "source_path": str(source),
                "content_sha256": current.get("content_sha256"), "reason": "artifact content unchanged since run baseline"}
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(source, destination)
        provenance["archive_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    except OSError as exc:
        return {**result, "status": "invalid", "source_path": str(source), "validation_errors": [f"archive failed: {exc}"]}
    validation = validate_latency_artifact(destination, expected_run_id=run_id,
        expected_producer_pid=sut_pid, expected_producer_start_time_ticks=sut_start_time_ticks)
    provenance["producer_completion_verified"] = validation["valid"]
    provenance["clock_domain"] = validation.get("clock_domain")
    if provenance["archive_sha256"] != provenance["source_sha256"]:
        validation["valid"] = False
        validation["validation_errors"].append("source changed while artifact was archived")
    controlled = (managed and mode == "managed" and graceful_finalization and validation["valid"]
                  and not validation.get("legacy")
                  and validation.get("clock_domain") == LATENCY_CLOCK_DOMAIN)
    if validation["record_count"] == 0:
        status, reason = "invalid", "artifact contains no data records"
    elif validation["valid"] and controlled:
        status, reason = "supported", None
    elif mode == "attached" and not config["required"]:
        status = "partial"
        reason = "attached mode has no controlled finalization; archived artifact is diagnostic only"
    else:
        status, reason = "invalid", "artifact contract or controlled finalization is invalid"
    if validation.get("legacy") and validation["valid"]:
        status = "invalid" if config["required"] else "partial"
        reason = "legacy completion has no producer clock domain; diagnostic only"
    return {**result, **validation, "status": status, "source_path": str(source),
            "archive_path": str(destination), "content_sha256": current.get("content_sha256"),
            **({"reason": reason} if reason else {})}


class Ros2Adapter:
    """Collect ROS environment and passive CLI graph evidence.

    Stable minimal interface:
      ``doctor()`` checks capabilities without inventing measurements.
      ``run(config)`` returns a JSON-serialisable result, including readiness.
    ``runner`` is injectable for tests and downstream tool integration.
    """

    ENV_KEYS = (
        "ROS_DISTRO",
        "ROS_VERSION",
        "RMW_IMPLEMENTATION",
        "ROS_DOMAIN_ID",
        "ROS_LOCALHOST_ONLY",
        "ROS_SECURITY_ENABLE",
        "ROS_SECURITY_STRATEGY",
        "ROS_DISCOVERY_SERVER",
        "RMW_FASTRTPS_USE_SHM",
        "AMENT_PREFIX_PATH",
    )
    SUT_ENV_ALLOWLIST = frozenset(ENV_KEYS)
    TRUSTED_BASE_ENV_KEYS = frozenset(("PATH", "HOME", "LANG", "LC_ALL", "TZ"))
    UNSAFE_ENV_KEYS = frozenset(("PYTHONPATH", "PYTHONHOME", "LD_PRELOAD", "LD_LIBRARY_PATH"))
    TRUSTED_INTERPRETER_ROOTS = (Path("/usr/bin"), Path("/usr/local/bin"))
    MAX_LAUNCHER_BYTES = 4096
    TRACEPOINT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(?::[A-Za-z_][A-Za-z0-9_]*)+$")

    DEFAULT_READINESS_TIMEOUT_SECONDS = 10.0
    DEFAULT_READINESS_POLL_INTERVAL_SECONDS = 0.2
    MAX_GRAPH_QUERY_EVIDENCE = 64
    MAX_GRAPH_QUERY_DETAIL_CHARS = 1024

    @classmethod
    def valid_environment_value(cls, key: str, value: str) -> bool:
        if not isinstance(value, str) or "\x00" in value or "\n" in value:
            return False
        if key == "ROS_DOMAIN_ID":
            return value.isascii() and value.isdigit() and 0 <= int(value) <= 232
        if key == "ROS_VERSION":
            return value == "2"
        if key == "ROS_LOCALHOST_ONLY":
            return value in ("0", "1")
        if key in ("ROS_DISTRO", "RMW_IMPLEMENTATION"):
            return re.fullmatch(r"[A-Za-z0-9_]+", value) is not None
        if key == "ROS_SECURITY_ENABLE":
            return value.lower() in ("true", "false")
        if key == "ROS_SECURITY_STRATEGY":
            return value in ("Enforce", "Permissive")
        if key == "AMENT_PREFIX_PATH":
            return bool(value) and all(Path(item).is_absolute() for item in value.split(os.pathsep))
        if key in cls.TRUSTED_BASE_ENV_KEYS:
            return bool(value) and (key not in ("PATH", "HOME") or all(
                Path(item).is_absolute() for item in value.split(os.pathsep)))
        return key in cls.SUT_ENV_ALLOWLIST and re.fullmatch(r"[A-Za-z0-9_./,:@+-]+", value) is not None

    @classmethod
    def sanitized_environment(cls, source: Mapping[str, str], *, include_ros: bool = True) -> dict[str, str]:
        allowed: set[str] = set(cls.TRUSTED_BASE_ENV_KEYS)
        if include_ros:
            allowed.update(cls.ENV_KEYS)
        return {key: value for key, value in source.items()
                if key in allowed and cls.valid_environment_value(key, value)}

    def __init__(self, runner: CommandRunner | None = None, environ: Mapping[str, str] | None = None,
                  proc_root: str | Path = "/proc", ros2_executable: str | None = None,
                  trusted_path: str | None = None, ros2_prefix: str | None = None):
        self._run = runner or subprocess.run
        source_env = dict(os.environ if environ is None else environ)
        path = trusted_path if trusted_path is not None else source_env.get("PATH", os.defpath)
        self._env = self.sanitized_environment(source_env)
        self._env["PATH"] = path if self.valid_environment_value("PATH", path) else os.defpath
        self._trusted_runtime = self._configure_trusted_runtime(ros2_prefix, ros2_executable)
        if ros2_prefix is not None:
            self._ros2_executable = self._trusted_runtime.get("executable") \
                if self._trusted_runtime["status"] == "ready" else None
            if self._trusted_runtime["status"] == "ready":
                self._env["PYTHONPATH"] = os.pathsep.join(self._trusted_runtime["python_paths"])
                if self._trusted_runtime["library_paths"]:
                    self._env["LD_LIBRARY_PATH"] = os.pathsep.join(
                        self._trusted_runtime["library_paths"])
        else:
            configured = Path(ros2_executable).expanduser() if ros2_executable else None
            if configured is not None:
                self._ros2_executable = str(configured.resolve()) if configured.is_absolute() else None
            elif runner is not None:
                # An injected runner is itself a trusted collector boundary used by
                # tests/embedders; still pass an absolute command identity to it.
                self._ros2_executable = "/trusted/ros2"
            else:
                found = shutil.which("ros2", path=path)
                self._ros2_executable = str(Path(found).resolve()) if found else None
            if self._ros2_executable is not None and runner is None:
                candidate = Path(self._ros2_executable)
                if not candidate.is_file() or not os.access(candidate, os.X_OK):
                    self._ros2_executable = None
        self._proc_root = Path(proc_root)

    @staticmethod
    def _canonical_existing(path: Path, *, directory: bool) -> Path | None:
        if not path.is_absolute():
            return None
        try:
            canonical = path.resolve(strict=True)
        except OSError:
            return None
        if canonical != path or (directory and not canonical.is_dir()) or (
                not directory and not canonical.is_file()):
            return None
        return canonical

    @staticmethod
    def _beneath(path: Path, root: Path) -> bool:
        try:
            path.relative_to(root)
        except ValueError:
            return False
        return path != root

    @classmethod
    def _read_regular_file_bounded(cls, path: Path, limit: int) -> bytes | None:
        """Read a canonical regular file without following a replacement symlink."""
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(path, flags)
            try:
                metadata = os.fstat(fd)
                current = path.lstat()
                if (not stat.S_ISREG(metadata.st_mode)
                        or (metadata.st_dev, metadata.st_ino) != (current.st_dev, current.st_ino)
                        or metadata.st_size > limit):
                    return None
                data = os.read(fd, limit + 1)
                return data if len(data) <= limit else None
            finally:
                os.close(fd)
        except OSError:
            return None

    @classmethod
    def _trusted_python_interpreter(cls, launcher: Path) -> tuple[Path, str, str | None] | None:
        data = cls._read_regular_file_bounded(launcher, cls.MAX_LAUNCHER_BYTES)
        if data is None or not data.startswith(b"#!"):
            return None
        first_line = data.split(b"\n", 1)[0]
        try:
            shebang = first_line[2:].decode("ascii")
        except UnicodeDecodeError:
            return None
        # Arguments (including /usr/bin/env forms) make interpreter identity ambiguous.
        if not shebang or shebang != shebang.strip() or any(char.isspace() for char in shebang):
            return None
        declared = Path(shebang)
        if not declared.is_absolute():
            return None
        try:
            interpreter = declared.resolve(strict=True)
            metadata = interpreter.stat()
        except OSError:
            return None
        roots: list[Path] = []
        for root in cls.TRUSTED_INTERPRETER_ROOTS:
            try:
                roots.append(root.resolve(strict=True))
            except OSError:
                continue
        if (not stat.S_ISREG(metadata.st_mode) or not os.access(interpreter, os.X_OK)
                or not any(cls._beneath(interpreter, root) for root in roots)):
            return None
        match = re.fullmatch(r"python(\d+)\.(\d+)", interpreter.name)
        if match is None:
            return None
        return interpreter, f"{match.group(1)}.{match.group(2)}", shebang

    @classmethod
    def _interpreter_multiarch(cls, interpreter: Path) -> str | None:
        data = cls._read_regular_file_bounded(interpreter, 64 * 1024 * 1024)
        if data is None or len(data) < 20 or data[:4] != b"\x7fELF" or data[5] not in (1, 2):
            return None
        byteorder = "little" if data[5] == 1 else "big"
        machine = int.from_bytes(data[18:20], byteorder)
        return {3: "i386-linux-gnu", 40: "arm-linux-gnueabihf",
                62: "x86_64-linux-gnu", 183: "aarch64-linux-gnu"}.get(machine)

    def _configure_trusted_runtime(self, prefix_value: str | None,
                                   executable_value: str | None) -> dict[str, Any]:
        result: dict[str, Any] = {"status": "unconfigured", "prefix": None,
                                  "executable": None, "python_paths": [],
                                  "library_paths": [], "interpreter": None,
                                  "shebang_interpreter": None, "python_version": None,
                                  "identity": None,
                                  "reason": "ros2_prefix is not configured"}
        if prefix_value is None:
            return result
        if not isinstance(prefix_value, str):
            return {**result, "status": "invalid",
                    "reason": "ros2_prefix must be an absolute canonical directory"}
        prefix = self._canonical_existing(Path(prefix_value).expanduser(), directory=True)
        if prefix is None:
            return {**result, "status": "invalid",
                    "reason": "ros2_prefix must be an absolute canonical non-symlink directory"}
        result["prefix"] = str(prefix)

        executable_path = (Path(executable_value).expanduser() if executable_value
                           else prefix / "bin" / "ros2")
        executable = self._canonical_existing(executable_path, directory=False)
        if executable is None or not self._beneath(executable, prefix) or not os.access(executable, os.X_OK):
            return {**result, "status": "unavailable",
                    "reason": "ros2 executable is unavailable or outside ros2_prefix"}

        interpreter_evidence = self._trusted_python_interpreter(executable)
        if interpreter_evidence is None:
            return {**result, "status": "unavailable", "executable": str(executable),
                    "reason": "ros2 executable has no supported trusted Python shebang"}
        interpreter, python_version, shebang_interpreter = interpreter_evidence
        version = f"python{python_version}"
        python_candidates = (prefix / "lib" / version / "site-packages",
                             prefix / "local" / "lib" / version / "dist-packages")
        python_paths = [candidate for candidate in (
            self._canonical_existing(path, directory=True) for path in python_candidates)
            if candidate is not None and self._beneath(candidate, prefix)]
        package_available = False
        for path in python_paths:
            for package, directory in ((path / "ros2cli", True), (path / "ros2cli.py", False)):
                canonical_package = self._canonical_existing(package, directory=directory)
                if canonical_package is not None and self._beneath(canonical_package, prefix):
                    package_available = True
        if not package_available:
            return {**result, "status": "unavailable", "executable": str(executable),
                    "reason": f"ros2cli package is unavailable for {version}"}

        library_candidates = [prefix / "lib"]
        multiarch = self._interpreter_multiarch(interpreter)
        if multiarch is not None:
            library_candidates.append(prefix / "lib" / multiarch)
        library_paths = [candidate for candidate in (
            self._canonical_existing(path, directory=True) for path in library_candidates)
            if candidate is not None and self._beneath(candidate, prefix)]
        identity_value = {"prefix": str(prefix), "executable": str(executable),
                          "interpreter": str(interpreter),
                          "shebang_interpreter": shebang_interpreter,
                          "python_paths": [str(path) for path in python_paths],
                          "library_paths": [str(path) for path in library_paths],
                          "python_version": python_version}
        identity = hashlib.sha256(json.dumps(identity_value, sort_keys=True,
                                              separators=(",", ":")).encode()).hexdigest()
        return {"status": "ready", **identity_value, "identity": identity, "reason": None}

    def trusted_runtime(self) -> dict[str, Any]:
        return dict(self._trusted_runtime)

    def _ros2_args(self, *args: str) -> tuple[str, ...] | None:
        return (self._ros2_executable, *args) if self._ros2_executable else None

    @staticmethod
    def _record(name: str, value: Any = None, *, method: str, availability: str, detail: str | None = None) -> dict[str, Any]:
        record: dict[str, Any] = {
            "name": name,
            "value": value,
            "method": method,
            "measurement_kind": "observation",
            "measurement_lane": "passive_business_observation",
            "metric_availability": availability,
            "availability": availability,
        }
        if detail:
            record["detail"] = detail
        return record

    def _command(
        self,
        args: Sequence[str],
        name: str,
        *,
        timeout: float | None = None,
        deadline: float | None = None,
    ) -> dict[str, Any]:
        command_timeout = 10.0 if timeout is None else max(0.0, float(timeout))
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return self._record(name, method="subprocess", availability="unsupported", detail="deadline expired before command start")
            command_timeout = min(command_timeout, remaining)
        if command_timeout <= 0:
            return self._record(name, method="subprocess", availability="unsupported", detail="command timeout expired before command start")
        try:
            completed = self._run(list(args), capture_output=True, text=True, check=False, timeout=command_timeout, env=self._env)
        except (FileNotFoundError, OSError, subprocess.TimeoutExpired) as exc:
            return self._record(name, method="subprocess", availability="unsupported", detail=str(exc))
        if deadline is not None and time.monotonic() > deadline:
            return self._record(name, method="subprocess", availability="unsupported", detail="deadline expired before command completed")
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "command failed").strip()
            return self._record(name, method="subprocess", availability="unsupported", detail=detail)
        return self._record(name, (completed.stdout or "").strip(), method="subprocess", availability="supported")

    def _ros2_command(self, args: Sequence[str], name: str, **kwargs: Any) -> dict[str, Any]:
        command = self._ros2_args(*args)
        if command is None:
            return self._record(name, method="subprocess", availability="unsupported",
                                detail="trusted absolute ros2 executable is unavailable")
        return self._command(command, name, **kwargs)

    def environment(self) -> list[dict[str, Any]]:
        values = {key: self._env[key] for key in self.ENV_KEYS if key in self._env}
        availability = "supported" if values else "unsupported"
        detail = None if values else "no ROS environment variables detected"
        return [self._record("ros_environment", values, method="process_environment", availability=availability, detail=detail)]

    @staticmethod
    def _not_available(record: dict[str, Any]) -> dict[str, Any]:
        """Use the explicit preflight vocabulary without changing old records."""
        if record.get("metric_availability") == "unsupported":
            record = dict(record)
            record["metric_availability"] = "not_available"
            record["availability"] = "not_available"
        return record

    def _preflight_command(self, args: Sequence[str], name: str) -> dict[str, Any]:
        return self._not_available(self._ros2_command(args, name))

    def _proc_maps(self, pid: int | None) -> list[dict[str, Any]]:
        if pid is None:
            return []
        try:
            text = (self._proc_root / str(pid) / "maps").read_text(errors="replace")
        except OSError:
            return []
        result: list[dict[str, Any]] = []
        for line in text.splitlines():
            fields = line.split(None, 5)
            if len(fields) != 6 or not fields[5].startswith("/"):
                continue
            try:
                offset = int(fields[2], 16)
                inode = int(fields[4])
            except ValueError:
                continue
            deleted = fields[5].endswith(" (deleted)")
            result.append({"path": fields[5].removesuffix(" (deleted)"),
                           "address": fields[0], "perms": fields[1],
                           "offset": offset, "device": fields[3], "inode": inode,
                           "deleted": deleted})
        return result

    @staticmethod
    def _parse_elf_provenance(data: bytes) -> dict[str, Any]:
        """Extract GNU build-id and SONAME from bounded ELF bytes."""
        empty = {"soname": None, "build_id": None, "elf_metadata_status": "missing",
                 "elf_metadata_reason": None}
        max_phnum, max_notes, max_dynamic, max_string = 128, 128, 4096, 16 * 1024 * 1024
        if len(data) > 64 * 1024 * 1024:
            return {"soname": None, "build_id": None, "elf_metadata_status": "failed",
                    "elf_metadata_reason": "file_too_large"}

        def fail(reason: str) -> dict[str, Any]:
            return {"soname": None, "build_id": None, "elf_metadata_status": "failed",
                    "elf_metadata_reason": reason}

        if len(data) < 16 or data[:4] != b"\x7fELF":
            return fail("not_elf")
        ei_class, ei_data = data[4], data[5]
        if ei_class not in (1, 2) or ei_data not in (1, 2):
            return fail("unsupported_elf_format")
        endian = "<" if ei_data == 1 else ">"
        is64 = ei_class == 2
        eh_size = 64 if is64 else 52
        ph_size = 56 if is64 else 32
        if len(data) < eh_size:
            return fail("truncated_elf_header")
        try:
            if is64:
                header = struct.unpack_from(endian + "16sHHIQQQIHHHHHH", data)
            else:
                header = struct.unpack_from(endian + "16sHHIIIIIHHHHHH", data)
            phoff, phentsize, phnum = header[5], header[9], header[10]
        except struct.error:
            return fail("malformed_elf_header")
        if phentsize < ph_size or phnum > max_phnum:
            return fail("invalid_program_header_table")
        if phoff > len(data) or phnum and phoff + phnum * phentsize > len(data):
            return fail("truncated_program_header_table")
        loads: list[tuple[int, int, int]] = []
        notes: list[bytes] = []
        dynamic: tuple[int, int] | None = None
        try:
            for index in range(phnum):
                pos = phoff + index * phentsize
                if is64:
                    p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(endian + "IIQQQQQQ", data, pos)
                else:
                    p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from(endian + "IIIIIIII", data, pos)
                if p_filesz > p_memsz:
                    return fail("file_segment_larger_than_memory_segment")
                if p_offset > len(data) or p_filesz > len(data) - p_offset:
                    return fail("truncated_program_segment")
                segment = data[p_offset:p_offset + p_filesz]
                if p_type == 1:
                    loads.append((p_vaddr, p_offset, p_filesz))
                elif p_type == 4:
                    if p_filesz > max_string:
                        return fail("oversized_note")
                    if len(notes) >= max_notes:
                        return fail("too_many_note_segments")
                    notes.append(segment)
                elif p_type == 2:
                    if p_filesz > (16 if is64 else 8) * max_dynamic:
                        return fail("oversized_dynamic_segment")
                    if dynamic is not None:
                        return fail("multiple_dynamic_segments")
                    dynamic = (p_offset, p_filesz)
        except struct.error:
            return fail("malformed_program_header")

        for index, (vaddr, offset, filesz) in enumerate(loads):
            for other_vaddr, _, other_filesz in loads[index + 1:]:
                if max(vaddr, other_vaddr) < min(vaddr + filesz, other_vaddr + other_filesz):
                    return fail("ambiguous_pt_load_overlap")

        def mapped_offset(address: int, length: int = 1) -> int | None:
            for vaddr, offset, filesz in loads:
                if vaddr <= address and address - vaddr <= filesz and length <= filesz - (address - vaddr):
                    return offset + address - vaddr
            return None

        build_ids: list[str] = []
        for segment in notes:
            cursor = 0
            while cursor < len(segment):
                if len(segment) - cursor < 12:
                    return fail("truncated_note")
                namesz, descsz, note_type = struct.unpack_from(endian + "III", segment, cursor)
                if namesz > max_string or descsz > max_string:
                    return fail("oversized_note")
                name_end = cursor + 12 + namesz
                desc_start = cursor + 12 + ((namesz + 3) & ~3)
                desc_end = desc_start + descsz
                end = desc_start + ((descsz + 3) & ~3)
                if name_end > len(segment) or desc_end > len(segment) or end > len(segment):
                    return fail("truncated_note")
                name = segment[cursor + 12:name_end].rstrip(b"\0")
                if name == b"GNU" and note_type == 3 and descsz:
                    build_ids.append(segment[desc_start:desc_end].hex())
                cursor = end
        if build_ids and len(set(build_ids)) != 1:
            return fail("conflicting_build_ids")

        soname: str | None = None
        if dynamic is not None:
            dyn_offset, dyn_size = dynamic
            entry_size = 16 if is64 else 8
            if dyn_size > entry_size * max_dynamic or dyn_size % entry_size:
                return fail("invalid_dynamic_segment")
            strtab_addr = strtab_size = None
            dynamic_tags: dict[int, int] = {}
            soname_offsets: list[int] = []
            terminated = False
            for pos in range(dyn_offset, dyn_offset + dyn_size, entry_size):
                tag, value = struct.unpack_from(endian + ("qQ" if is64 else "iI"), data, pos)
                if tag == 0:
                    terminated = True
                    break
                if tag in (5, 10):
                    previous = dynamic_tags.get(tag)
                    if previous is not None and previous != value:
                        return fail("conflicting_dynamic_tags")
                    dynamic_tags[tag] = value
                    if tag == 5: strtab_addr = value
                    else: strtab_size = value
                elif tag == 14: soname_offsets.append(value)
            if not terminated:
                return fail("missing_dynamic_terminator")
            if soname_offsets:
                if strtab_addr is None or strtab_size is None or strtab_size > max_string:
                    return fail("invalid_string_table")
                strtab_file = mapped_offset(strtab_addr, strtab_size)
                if strtab_file is None or strtab_file + strtab_size > len(data):
                    return fail("invalid_soname_offset")
                sonames: list[str] = []
                for soname_offset in soname_offsets:
                    if soname_offset >= strtab_size:
                        return fail("invalid_soname_offset")
                    raw = data[strtab_file + soname_offset:strtab_file + strtab_size]
                    nul = raw.find(b"\0")
                    if nul < 0:
                        return fail("unterminated_soname")
                    try:
                        value = raw[:nul].decode("utf-8")
                    except UnicodeDecodeError:
                        return fail("invalid_soname_utf8")
                    if not value:
                        return fail("empty_soname")
                    sonames.append(value)
                if len(set(sonames)) != 1:
                    return fail("conflicting_sonames")
                soname = sonames[0]
        result = dict(empty)
        result["build_id"] = build_ids[0] if build_ids else None
        result["soname"] = soname
        result["elf_metadata_status"] = "ok" if build_ids or soname else "missing"
        return result

    @staticmethod
    def _mapped_file_identity(mapping: Mapping[str, Any], *, max_bytes: int | None = None,
                              remaining_bytes: list[int] | None = None) -> dict[str, Any]:
        """Hash one open file description and bind it to the maps identity."""
        item: dict[str, Any] = {"sha256": None, "mapped_identity_matches": False,
                                "fd_device": None, "fd_inode": None, "fd_size": None,
                                "soname": None, "build_id": None,
                                "elf_metadata_status": "missing", "elf_metadata_reason": None}
        if mapping.get("deleted"):
            item["detail"] = "deleted mapping cannot be verified against the current path"
            return item
        fd: int | None = None
        try:
            fd = os.open(str(mapping["path"]), os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
                         getattr(os, "O_NOFOLLOW", 0))
            before = os.fstat(fd)
            item.update({"fd_device": f"{os.major(before.st_dev):02x}:{os.minor(before.st_dev):02x}",
                         "fd_inode": before.st_ino, "fd_size": before.st_size})
            if max_bytes is not None and before.st_size > max_bytes:
                item["detail"] = "mapped file exceeds per-file hashing limit"
                return item
            if remaining_bytes is not None and before.st_size > remaining_bytes[0]:
                item["detail"] = "mapped file exceeds aggregate hashing limit"
                return item
            if remaining_bytes is not None:
                remaining_bytes[0] -= before.st_size
            try:
                map_major, map_minor = (int(part, 16) for part in str(mapping["device"]).split(":", 1))
                device_matches = (os.major(before.st_dev), os.minor(before.st_dev)) == (map_major, map_minor)
            except (TypeError, ValueError):
                device_matches = False
            item["mapped_identity_matches"] = device_matches and before.st_ino == mapping["inode"]
            digest = hashlib.sha256()
            data = bytearray()
            expected_size = before.st_size
            remaining = expected_size
            too_large = False
            while remaining:
                chunk = os.read(fd, min(1024 * 1024, remaining))
                if not chunk:
                    item["detail"] = "mapped file shrank while hashing"
                    return item
                digest.update(chunk)
                if len(data) <= 64 * 1024 * 1024:
                    data.extend(chunk)
                    too_large = len(data) > 64 * 1024 * 1024
                remaining -= len(chunk)
            after = os.fstat(fd)
            if after.st_size > expected_size:
                item["detail"] = "mapped file grew while hashing"
                return item
            if any(getattr(before, field) != getattr(after, field)
                   for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
                item["mapped_identity_matches"] = False
                item["detail"] = "mapped file identity was unstable while hashing"
                return item
            item["sha256"] = digest.hexdigest()
            metadata = {"soname": None, "build_id": None, "elf_metadata_status": "failed",
                        "elf_metadata_reason": "file_too_large"} if too_large else Ros2Adapter._parse_elf_provenance(bytes(data))
            item.update(metadata)
            if not item["mapped_identity_matches"]:
                item["detail"] = "opened file device/inode does not match the mapped object"
        except OSError as exc:
            item["detail"] = f"mapped file is unreadable: {exc}"
        finally:
            if fd is not None:
                os.close(fd)
        return item

    def _preflight_pid_identity(self, pid: int | None) -> int | None:
        if pid is None:
            return None
        try:
            raw = (self._proc_root / str(pid) / "stat").read_text(encoding="utf-8")
            close = raw.rfind(")")
            fields = raw[close + 1:].split() if close >= 0 else raw.split()[2:]
            return int(fields[19])
        except (OSError, ValueError, IndexError):
            return None

    @staticmethod
    def _tracepoint_names(value: Any) -> list[str]:
        names = []
        for line in str(value or "").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            token = stripped.split()[0]
            if Ros2Adapter.TRACEPOINT_RE.fullmatch(token):
                names.append(token)
        return sorted(set(names))

    @staticmethod
    def _expected_library_specs(config: Mapping[str, Any]) -> list[dict[str, Any]]:
        value = config.get("required_runtime_libraries", [])
        if not isinstance(value, list):
            raise ValueError("required_runtime_libraries must be an array")
        specs: list[dict[str, Any]] = []
        ids: set[str] = set()
        selectors: set[tuple[str, str]] = set()
        for item in value:
            if not isinstance(item, Mapping):
                raise ValueError("required runtime library declaration must be an object")
            spec = dict(item)
            stable_id = spec.get("id")
            if not isinstance(stable_id, str) or not stable_id or stable_id in ids:
                raise ValueError("required runtime library id must be non-empty and unique")
            selected = [(key, spec[key]) for key in ("name", "path")
                        if isinstance(spec.get(key), str) and spec[key]]
            constraints = [key for key in ("sha256", "build_id", "soname")
                           if isinstance(spec.get(key), str) and spec[key]]
            if len(selected) != 1 or not constraints:
                raise ValueError("required runtime library needs exactly one selector and an identity constraint")
            selector_kind, selector_value = selected[0]
            selector_basename = Path(selector_value).name
            if ((selector_kind == "path" and ("basename", selector_basename) in selectors)
                    or (selector_kind == "name"
                        and ("path_basename", selector_basename) in selectors)):
                raise ValueError("ambiguous required runtime library selector")
            tokens = ({("path", selector_value), ("path_basename", selector_basename)}
                      if selector_kind == "path" else {("basename", selector_basename)})
            if isinstance(spec.get("soname"), str):
                tokens.update({("soname", spec["soname"]), ("basename", spec["soname"])})
            collision_tokens = tokens - ({("path_basename", selector_basename)}
                                         if selector_kind == "path" else set())
            if selectors.intersection(collision_tokens):
                raise ValueError("ambiguous required runtime library selector")
            ids.add(stable_id)
            selectors.update(tokens)
            specs.append(spec)
        return specs

    @staticmethod
    def _apply_expected_identity(library: dict[str, Any], specs: Sequence[Mapping[str, Any]]) -> None:
        basename = Path(library["path"]).name
        matches = [spec for spec in specs
                   if spec.get("name") == basename or spec.get("path") == library["path"]]
        library.update(required=bool(matches), capability_verified=False,
                       expected_identity_match=False)
        if not matches or library.get("verification") != "identity_verified":
            return
        for spec in matches:
            checks = [expected == observed for expected, observed in (
                (spec.get("sha256"), library.get("sha256")),
                (spec.get("build_id"), library.get("build_id")),
                (spec.get("soname"), library.get("soname"))) if expected is not None]
            if checks and all(checks):
                library.update(capability_verified=True, expected_identity_match=True)
                return

    def preflight(self, config: Mapping[str, Any] | None = None) -> dict[str, Any]:
        """Collect passive, repeatable P0a evidence; never creates a ROS entity."""
        config = config or {}
        pid_value = config.get("pid")
        pid = int(pid_value) if pid_value is not None else None
        identity_start = self._preflight_pid_identity(pid)
        env: dict[str, Any] = {key: self._env.get(key) for key in ("ROS_DISTRO", "RMW_IMPLEMENTATION")}
        prefixes: list[str] = []
        for key in ("ROS_PREFIX_PATH", "AMENT_PREFIX_PATH"):
            prefixes.extend(x for x in self._env.get(key, "").split(os.pathsep) if x)
        env["prefixes"] = prefixes
        packages = config.get("packages", ["rmw_fastrtps_cpp"])
        if isinstance(packages, str):
            packages = [packages]
        pkg_records = [self._preflight_command(("pkg", "prefix", str(pkg)), "ros2_pkg_prefix")
                       for pkg in packages if str(pkg)]
        specs = self._expected_library_specs(config)
        max_selected = int(config.get("max_selected_library_count", 64))
        max_file_bytes = int(config.get("max_library_file_bytes", 64 * 1024 * 1024))
        max_aggregate_bytes = int(config.get("max_library_aggregate_bytes", 256 * 1024 * 1024))
        if min(max_selected, max_file_bytes, max_aggregate_bytes) < 0:
            raise ValueError("library hashing limits must be non-negative")
        maps = self._proc_maps(pid)
        library_maps = [x for x in maps if any(token in Path(x["path"]).name.lower()
                          for token in ("fastdds", "fastrtps", "rmw_")) or any(
                              spec.get("name") == Path(x["path"]).name or spec.get("path") == x["path"]
                              for spec in specs)]
        selected_maps: dict[tuple[str, int], dict[str, Any]] = {}
        for mapping in library_maps:
            key = (mapping["device"], mapping["inode"])
            if key not in selected_maps or ("x" in mapping["perms"] and "x" not in selected_maps[key]["perms"]):
                selected_maps[key] = mapping
        selected_items = list(selected_maps.values())
        required_candidates = {
            (mapping["device"], mapping["inode"])
            for mapping in selected_items
            if any(spec.get("name") == Path(mapping["path"]).name
                   or spec.get("path") == mapping["path"] for spec in specs)
        }
        if len(required_candidates) > max_selected:
            raise ValueError("selected library limit would omit a required runtime library")
        required_keys = {
            (mapping["device"], mapping["inode"])
            for mapping in selected_items
            if any(spec.get("name") == Path(mapping["path"]).name
                   or spec.get("path") == mapping["path"] for spec in specs)
        }
        prioritized = [mapping for mapping in selected_items
                       if (mapping["device"], mapping["inode"]) in required_keys]
        prioritized.extend(mapping for mapping in selected_items if mapping not in prioritized)
        limited_items = prioritized[:max_selected]
        remaining_bytes = [max_aggregate_bytes]
        libraries: list[dict[str, Any]] = []
        for mapping in limited_items:
            identity = self._mapped_file_identity(mapping, max_bytes=max_file_bytes,
                                                  remaining_bytes=remaining_bytes)
            if mapping["deleted"]:
                status, verification = "unverifiable_deleted", "unverifiable_deleted"
            else:
                status, verification = "candidate", "unverified"
            libraries.append({"path": mapping["path"], "role": "fast_dds_or_rmw",
                              "status": status, "verification": verification,
                              "observed_in_proc_maps": True, "mapping": dict(mapping),
                              "soname": None, "build_id": None, **identity})
        trace = [self._preflight_command(("trace", "-h"), "ros2_tracing_command"),
                 self._preflight_command(("trace", "--list"), "ros2_tracing_tracepoints")]

        required_value = config.get("required_tracepoints", [])
        if isinstance(required_value, str):
            required_value = [required_value]
        required_tracepoints = [str(item) for item in required_value
                                if self.TRACEPOINT_RE.fullmatch(str(item))]
        command_available = trace[0]["metric_availability"] == "supported"
        session_command_usable = False
        tracepoints_enumerated = trace[1]["metric_availability"] == "supported"
        tracepoint_names = self._tracepoint_names(trace[1].get("value")) if tracepoints_enumerated else []
        missing_tracepoints = [item for item in required_tracepoints if item not in tracepoint_names]
        required_tracepoints_present = (bool(required_tracepoints) and tracepoints_enumerated
                                        and not missing_tracepoints)

        identity_end = self._preflight_pid_identity(pid)
        identity_valid = pid is not None and identity_start is not None and identity_start == identity_end
        if pid is None:
            identity_reason = "target_pid_unset"
        elif identity_start is None:
            identity_reason = "identity_unavailable_at_start"
        elif identity_end is None:
            identity_reason = "process_exited_or_identity_unavailable_at_end"
        elif identity_start != identity_end:
            identity_reason = "pid_reused_or_process_identity_changed"
        else:
            identity_reason = None
        for library in libraries:
            mapping = library["mapping"]
            if (identity_valid and "x" in mapping["perms"] and library["mapped_identity_matches"]
                    and library["sha256"]):
                library.update({"status": "identity_verified", "verification": "identity_verified"})
            elif library["status"] != "unverifiable_deleted":
                library.update({"status": "candidate", "verification": "unverified"})
                if not identity_valid:
                    library["detail"] = "runtime resolution is unverified because process identity is unstable"

        specs = self._expected_library_specs(config)
        for library in libraries:
            self._apply_expected_identity(library, specs)
        required_matches: list[dict[str, Any]] = []
        for spec in specs:
            candidates = [library for library in libraries
                          if spec.get("name") == Path(library["path"]).name
                          or spec.get("path") == library["path"]]
            match: dict[str, Any] = {"id": spec["id"], "matched": False}
            if len(candidates) > 1:
                match["reason"] = "ambiguous_runtime_match"
                for library in candidates:
                    library.update(capability_verified=False,
                                   reason="ambiguous_runtime_match")
            elif len(candidates) == 1 and candidates[0].get("capability_verified"):
                match["matched"] = True
            required_matches.append(match)
        resolution_available = identity_valid and bool(maps)
        resolution = self._record(
            "dynamic_library_resolution",
            {"source": f"{self._proc_root}/{pid}/maps" if pid is not None else None,
             "evidence": "mapped_objects_only", "libraries": libraries} if resolution_available else None,
            method="proc_pid_maps", availability="supported" if resolution_available else "not_available",
            detail=None if resolution_available else
            "runtime library resolution requires a stable PID identity and /proc/PID/maps",
        )
        value = {"schema": "mw_perf.ros2.preflight.v1", "pid": pid,
                 "ros2_command": {"path": self._ros2_executable,
                                  "trusted": self._ros2_executable is not None
                                  and Path(self._ros2_executable).is_absolute()},
                 "environment": env, "trusted_runtime": self.trusted_runtime(),
                 "ros2_pkg_prefix": pkg_records,
                 "process_identity": {"start_ticks_before": identity_start,
                                      "start_ticks_after": identity_end,
                                      "valid": identity_valid, "reason": identity_reason},
                 "process": {"maps": maps, "dynamic_library_resolution": resolution, "resolved_libraries": libraries},
                 "ros2_tracing": {"commands": trace,
                                  "command_available": command_available,
                                  "session_command_usable": session_command_usable,
                                  "tracepoints_enumerated": tracepoints_enumerated,
                                  "required_tracepoints_present": required_tracepoints_present,
                                  "required_tracepoints": required_tracepoints,
                                  "missing_required_tracepoints": missing_tracepoints,
                                  "available": command_available and session_command_usable
                                  and tracepoints_enumerated and required_tracepoints_present,
                                  "tracepoints": tracepoint_names if tracepoints_enumerated else None},
                 "middleware_libraries": libraries,
                 "required_runtime_libraries": {"configured": bool(specs),
                                                  "all_capability_verified": bool(specs) and all(
                                                      item["matched"] for item in required_matches),
                                                  "matches": required_matches},
                 "claims": {"exact": False, "abi_inferred_from_version_name": False}}
        available = self._ros2_executable is not None and bool(
            env["ROS_DISTRO"] or env["RMW_IMPLEMENTATION"] or maps or libraries)
        return self._record("ros2_preflight", value, method="passive_preflight",
                            availability="supported" if available else "not_available",
                            detail=None if available else "ROS process/environment evidence unavailable")

    def graph_snapshot(
        self,
        *,
        timeout: float | None = None,
        deadline: float | None = None,
        include_topic_info: bool = True,
    ) -> dict[str, Any]:
        snapshot: dict[str, Any] = {"timestamp": time.time(), "nodes": [], "topics": [],
                                    "topic_info": {}, "query_evidence": [],
                                    "query_evidence_omitted": 0}
        query_records: list[tuple[tuple[str, ...], dict[str, Any]]] = []

        def query(args: tuple[str, ...], name: str) -> dict[str, Any]:
            record = self._ros2_command(args, name, timeout=timeout, deadline=deadline)
            if record.get("detail"):
                record = dict(record)
                record["detail"] = str(record["detail"])[:self.MAX_GRAPH_QUERY_DETAIL_CHARS]
            query_records.append((args, record))
            return record

        records = [
            query(("node", "list", "--no-daemon"), "ros2_node_list"),
            query(("topic", "list", "--no-daemon"), "ros2_topic_list"),
        ]
        node_record, topic_record = records
        if node_record["metric_availability"] == "supported":
            snapshot["nodes"] = [line.strip() for line in str(node_record["value"]).splitlines() if line.strip()]
        if topic_record["metric_availability"] == "supported":
            snapshot["topics"] = [line.strip() for line in str(topic_record["value"]).splitlines() if line.strip()]
            for topic in snapshot["topics"] if include_topic_info else ():
                info = query(("topic", "info", "-v", "--no-daemon", topic),
                             "ros2_topic_info")
                snapshot["topic_info"][topic] = info
        evidence = []
        for args, record in query_records[:self.MAX_GRAPH_QUERY_EVIDENCE]:
            item = {"command": list(self._ros2_args(*args) or args), "name": record["name"],
                    "metric_availability": record["metric_availability"]}
            if record.get("detail"):
                item["detail"] = str(record["detail"])[:self.MAX_GRAPH_QUERY_DETAIL_CHARS]
            evidence.append(item)
        snapshot["query_evidence"] = evidence
        snapshot["query_evidence_omitted"] = max(
            0, len(query_records) - self.MAX_GRAPH_QUERY_EVIDENCE)
        availability = ("supported" if query_records and all(
            record["metric_availability"] == "supported" for _, record in query_records)
            else "unsupported")
        failures = sum(record["metric_availability"] != "supported"
                       for _, record in query_records)
        detail = (f"CLI graph query failed ({failures} of {len(query_records)} commands)"
                  if availability == "unsupported" else None)
        return self._record("ros_graph_snapshot", snapshot, method="ros2_cli",
                            availability=availability, detail=detail)

    def doctor(self) -> dict[str, Any]:
        cli = self._ros2_command(("-h",), "ros2_cli")
        rmw = self._env.get("RMW_IMPLEMENTATION")
        runtime = self._record("ros2_trusted_runtime", self.trusted_runtime(),
                               method="trusted_prefix",
                               availability="supported" if self._trusted_runtime["status"] == "ready"
                               else "unsupported")
        return {"adapter": "ros2", "status": "ready" if cli["metric_availability"] == "supported" else "degraded", "observations": self.environment() + [cli, self._record("rmw_implementation", rmw, method="process_environment", availability="supported" if rmw else "unsupported", detail=None if rmw else "RMW_IMPLEMENTATION is unset"), runtime]}

    @staticmethod
    def _process_ready(pid: int) -> dict[str, Any]:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return {"name": "process_alive", "ok": False, "metric_availability": "supported", "detail": "process is not alive"}
        except PermissionError:
            return {"name": "process_alive", "ok": True, "metric_availability": "estimated", "detail": "process exists but access is restricted"}
        return {"name": "process_alive", "ok": True, "metric_availability": "supported"}

    @staticmethod
    def _pid_identity(pid: int) -> int | None:
        """Return Linux process start ticks without creating any ROS entity."""
        try:
            raw = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
            close = raw.rfind(")")
            fields = raw[close + 1:].split() if close >= 0 else raw.split()[2:]
            return int(fields[19])
        except (OSError, ValueError, IndexError):
            return None

    def readiness(self, config: Mapping[str, Any], snapshot: Mapping[str, Any] | None = None) -> dict[str, Any]:
        pid = config.get("pid")
        # Capture identity before accepting or obtaining graph evidence.  A PID
        # may be reused while a CLI query is in flight, so it is checked again
        # before every snapshot use and immediately before success.
        expected_identity = self._pid_identity(int(pid)) if pid is not None else None
        requirements = [
            (
                requirement["name"] if isinstance(requirement, Mapping) else str(requirement),
                int(requirement.get("count", 1)) if isinstance(requirement, Mapping) else 1,
            )
            for requirement in config.get("topics", [])
        ]
        readiness_config = config.get("readiness", {})
        if not isinstance(readiness_config, Mapping):
            readiness_config = {}
        timeout = max(0.0, float(config.get("readiness_timeout_seconds", readiness_config.get("timeout_seconds", self.DEFAULT_READINESS_TIMEOUT_SECONDS))))
        interval = max(0.0, float(config.get("readiness_poll_interval_seconds", readiness_config.get("poll_interval_seconds", self.DEFAULT_READINESS_POLL_INTERVAL_SECONDS))))
        started = time.monotonic()
        deadline = started + timeout
        attempts = 0
        first_seen: dict[str, float | None] = {name: None for name, _ in requirements}
        current_snapshot = snapshot
        checks: list[dict[str, Any]] = []
        failure_reason: str | None = None

        while True:
            attempts += 1
            checks = []
            if pid is not None:
                process_check = self._process_ready(int(pid))
                current_identity = self._pid_identity(int(pid))
                if not process_check["ok"]:
                    failure_reason = "process_exited"
                elif expected_identity is None or current_identity is None:
                    process_check = dict(process_check, ok=False, detail="PID identity is unavailable")
                    failure_reason = "pid_identity_unavailable"
                elif expected_identity is not None and current_identity != expected_identity:
                    process_check = dict(process_check, ok=False, detail="PID identity changed")
                    failure_reason = "pid_identity_changed"
                checks.append(process_check)

            if failure_reason is None and current_snapshot is None:
                current_snapshot = self.graph_snapshot(deadline=deadline, include_topic_info=False)

            if (failure_reason is None and requirements and current_snapshot is not None
                    and current_snapshot.get("metric_availability") == "unsupported"):
                failure_reason = "graph_query_failed"
                checks.append({"name": "graph_query", "ok": False,
                               "metric_availability": "unsupported",
                               "detail": current_snapshot.get("detail", "ROS graph query failed")})

            # Re-check after graph collection and before consuming its data.
            if pid is not None and failure_reason is None:
                process_check = self._process_ready(int(pid))
                current_identity = self._pid_identity(int(pid))
                if not process_check["ok"]:
                    failure_reason = "process_exited"
                elif expected_identity is None or current_identity is None:
                    failure_reason = "pid_identity_unavailable"
                elif current_identity != expected_identity:
                    failure_reason = "pid_identity_changed"

            topics = list((current_snapshot or {}).get("value", {}).get("topics", []))
            now = time.monotonic()
            for name, count in requirements:
                actual = topics.count(name)
                present = actual >= count
                if present and first_seen[name] is None:
                    first_seen[name] = max(0.0, now - started)
                checks.append({"name": "topic_present", "topic": name, "count": actual, "required_count": count, "ok": present, "metric_availability": "supported"})

            ok = all(check["ok"] for check in checks) if checks else True
            if ok and pid is not None:
                final_process = self._process_ready(int(pid))
                final_identity = self._pid_identity(int(pid))
                if not final_process["ok"]:
                    ok = False
                    failure_reason = "process_exited"
                elif expected_identity is None or final_identity is None:
                    ok = False
                    failure_reason = "pid_identity_unavailable"
                elif final_identity != expected_identity:
                    ok = False
                    failure_reason = "pid_identity_changed"
            if ok or failure_reason is not None:
                break
            if now >= deadline:
                failure_reason = "timeout"
                break
            time.sleep(min(interval, max(0.0, deadline - now)))
            current_snapshot = None

        elapsed = max(0.0, time.monotonic() - started)
        return {
            "ok": ok,
            "checks": checks,
            "evidence": {
                "attempts": attempts,
                "first_seen": first_seen,
                "elapsed": elapsed,
                "failure_reason": failure_reason,
            },
            "snapshot": current_snapshot,
        }

    def run(self, config: Mapping[str, Any] | None = None) -> dict[str, Any]:
        config = config or {}
        readiness = self.readiness(config)
        snapshot = readiness.pop("snapshot")
        if snapshot is None:
            snapshot = self._record("ros_graph_snapshot", {"timestamp": time.time(), "nodes": [],
                "topics": [], "topic_info": {}, "query_evidence": [],
                "query_evidence_omitted": 0}, method="ros2_cli", availability="unsupported",
                detail="readiness deadline expired before graph query")
        observations = self.environment() + [self._ros2_command(("-h",), "ros2_cli"), snapshot]
        if config.get("preflight") is True:
            observations.append(self.preflight(config))
        return {"adapter": "ros2", "status": "ok" if snapshot["metric_availability"] == "supported" else "degraded", "observations": observations, "readiness": readiness}


__all__ = ["Ros2Adapter", "archive_latency_artifact", "latency_artifact_config",
           "latency_artifact_snapshot", "resolve_latency_artifact", "validate_latency_artifact"]
