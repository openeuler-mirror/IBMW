"""Strict collector-owned validation for producer CSV latency summaries."""
from __future__ import annotations

import csv
import hashlib
import json
import math
import shutil
from pathlib import Path
from typing import Any, Mapping


SCHEMA = "mw_perf.latency.csv.v1"
CSV_FIELDS = ("label", "count", "min_us", "avg_us", "stddev_us", "p50_us",
              "p90_us", "p99_us", "p999_us", "max_us")


def _config(value: Any) -> dict[str, Any]:
    if value is None:
        return {"configured": False, "required": False}
    if not isinstance(value, Mapping):
        raise TypeError("artifacts.latency must be an object")
    required = ("path", "result_path", "lane", "expected_label", "expected_count")
    if any(name not in value for name in required):
        raise ValueError("artifacts.latency requires path, result_path, lane, expected_label and expected_count")
    if value.get("format") != "LatencyStats CSV" or value.get("schema") != SCHEMA:
        raise ValueError(f"artifacts.latency must declare LatencyStats CSV and {SCHEMA}")
    return {"configured": True, "required": value.get("required") is True, **dict(value)}


def _metadata(path: Path) -> dict[str, Any]:
    try:
        stat = path.stat()
        return {"exists": path.is_file(), "path": str(path), "mtime_ns": stat.st_mtime_ns,
                "size": stat.st_size, "inode": stat.st_ino,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    except OSError:
        return {"exists": False, "path": str(path), "mtime_ns": None, "size": None,
                "inode": None, "sha256": None}


def snapshot(value: Any) -> dict[str, Any]:
    config = _config(value)
    if not config["configured"]:
        return {"csv": None, "result": None}
    return {"csv": _metadata(Path(config["path"]).expanduser()),
            "result": _metadata(Path(config["result_path"]).expanduser())}


def validate_csv(path: Path, expected_label: str, expected_count: int) -> dict[str, Any]:
    errors: list[str] = []
    try:
        with path.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.reader(stream, strict=True))
    except (OSError, UnicodeError, csv.Error) as exc:
        return {"valid": False, "validation_errors": [f"csv unreadable: {exc}"]}
    if len(rows) != 2:
        return {"valid": False, "validation_errors": ["CSV must contain exactly one LatencyStats row"]}
    if tuple(rows[0]) != CSV_FIELDS:
        errors.append("CSV header does not match LatencyStats")
    if len(rows[1]) != len(CSV_FIELDS):
        return {"valid": False, "validation_errors": errors + ["CSV row column count mismatch"]}
    row = rows[1]
    if row[0] != expected_label:
        errors.append("CSV label mismatch")
    try:
        count = int(row[1])
        if str(count) != row[1] or count != expected_count or count < 0:
            errors.append("CSV count mismatch")
    except ValueError:
        count = None
        errors.append("CSV count is not an integer")
    values: dict[str, float] = {}
    for name, raw in zip(CSV_FIELDS[2:], row[2:]):
        try:
            value = float(raw)
            if not math.isfinite(value) or value < 0:
                raise ValueError
            values[name] = value
        except ValueError:
            errors.append(f"CSV {name} must be finite and non-negative")
    if len(values) == 8:
        ordered = [values[name] for name in ("min_us", "p50_us", "p90_us", "p99_us", "p999_us", "max_us")]
        if ordered != sorted(ordered):
            errors.append("CSV quantiles are not monotonic")
        if not values["min_us"] <= values["avg_us"] <= values["max_us"]:
            errors.append("CSV average is outside min/max")
    return {"valid": not errors, "schema": SCHEMA, "label": row[0], "record_count": count,
            "statistics": values, "validation_errors": errors}


def _validate_result(path: Path, *, run_id: str, pid: int, start_ticks: int, lane: str,
                     csv_sha256: str) -> tuple[bool, list[str], str | None]:
    errors: list[str] = []
    try:
        data = path.read_bytes()
        result = json.loads(data)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return False, [f"lane result unreadable: {exc}"], None
    identity = result.get("collector_managed") if isinstance(result, dict) else None
    expected = {"schema": "ibmw.abc.collector-managed.v1", "run_id": run_id,
                "producer_pid": pid, "producer_start_time_ticks": start_ticks,
                "lane": lane, "complete": True, "latency_csv_sha256": csv_sha256}
    if not isinstance(identity, dict) or identity != expected:
        errors.append("lane result collector identity mismatch")
    if not isinstance(result, dict) or result.get("valid") is not True or result.get("lane") != lane:
        errors.append("lane result is not a valid matching lane")
    return not errors, errors, hashlib.sha256(data).hexdigest()


def archive(value: Any, baseline: dict[str, Any], csv_destination: Path, result_destination: Path,
            *, run_id: str, mode: str, pid: int, start_ticks: int,
            graceful_finalization: bool) -> dict[str, Any]:
    config = _config(value)
    base = {"schema": SCHEMA, "required": config["required"], "status": "unsupported",
            "validation_errors": []}
    if not config["configured"]:
        return base
    current = snapshot(value)
    csv_meta, result_meta = current["csv"], current["result"]
    fresh = all(item and item.get("exists") for item in (csv_meta, result_meta)) and all(
        not baseline.get(name) or baseline[name].get("sha256") != current[name].get("sha256")
        for name in ("csv", "result"))
    if not fresh:
        return {**base, "status": "invalid" if config["required"] else "stale",
                "validation_errors": ["CSV or lane result is missing/stale"]}
    validation = validate_csv(Path(config["path"]), config["expected_label"], config["expected_count"])
    csv_sha = csv_meta["sha256"]
    identity_ok, identity_errors, result_sha = _validate_result(Path(config["result_path"]),
        run_id=run_id, pid=pid, start_ticks=start_ticks, lane=config["lane"], csv_sha256=csv_sha)
    validation["validation_errors"].extend(identity_errors)
    try:
        csv_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(config["path"], csv_destination)
        shutil.copy2(config["result_path"], result_destination)
        archive_csv_sha = hashlib.sha256(csv_destination.read_bytes()).hexdigest()
        archive_result_sha = hashlib.sha256(result_destination.read_bytes()).hexdigest()
    except OSError as exc:
        return {**base, "status": "invalid", "validation_errors": [f"archive failed: {exc}"]}
    controlled = mode == "managed" and graceful_finalization and validation["valid"] and identity_ok
    hashes_ok = csv_sha == archive_csv_sha and result_sha == archive_result_sha
    if not hashes_ok:
        validation["validation_errors"].append("source changed while artifacts were archived")
    provenance = {"run_id": run_id, "mode": mode, "sut_pid": pid,
        "sut_start_time_ticks": start_ticks, "lane": config["lane"],
        "source_path": str(Path(config["path"])), "source_sha256": csv_sha,
        "archive_sha256": archive_csv_sha, "lane_result_path": str(Path(config["result_path"])),
        "lane_result_sha256": archive_result_sha, "baseline": baseline, "final": current,
        "graceful_finalization": graceful_finalization, "lane_result_identity_verified": identity_ok}
    return {**base, **validation, "status": "supported" if controlled and hashes_ok else "invalid",
            "source_path": str(Path(config["path"])), "archive_path": str(csv_destination),
            "content_sha256": archive_csv_sha, "provenance": provenance}


def revalidate(value: Any, run_dir: Path, manifest: Mapping[str, Any]) -> bool:
    if not isinstance(value, Mapping) or value.get("status") != "supported" or value.get("schema") != SCHEMA:
        return False
    provenance = value.get("provenance")
    if not isinstance(provenance, Mapping):
        return False
    csv_path, result_path = run_dir / "raw/latency.csv", run_dir / "raw/lane_result.json"
    expected_count = value.get("record_count")
    pid, start_ticks = manifest.get("pid"), manifest.get("pid_start_time_ticks")
    if (not isinstance(expected_count, int) or isinstance(expected_count, bool)
            or not isinstance(pid, int) or isinstance(pid, bool)
            or not isinstance(start_ticks, int) or isinstance(start_ticks, bool)):
        return False
    validation = validate_csv(csv_path, str(value.get("label")), expected_count)
    try:
        csv_sha = hashlib.sha256(csv_path.read_bytes()).hexdigest()
        result_sha = hashlib.sha256(result_path.read_bytes()).hexdigest()
    except OSError:
        return False
    identity_ok, _, checked_result_sha = _validate_result(result_path,
        run_id=str(manifest.get("run_id")), pid=pid,
        start_ticks=start_ticks, lane=str(manifest.get("comparison_lane")),
        csv_sha256=csv_sha)
    final = provenance.get("final")
    freshness_ok = isinstance(final, Mapping) and all(
        isinstance(final.get(name), Mapping) and final[name].get("exists") is True
        and isinstance(final[name].get("mtime_ns"), int)
        and final[name]["mtime_ns"] >= int(float(manifest.get("start_time", 0)) * 1_000_000_000)
        for name in ("csv", "result"))
    return bool(validation["valid"] and identity_ok and freshness_ok
        and value.get("content_sha256") == csv_sha
        and provenance.get("source_sha256") == provenance.get("archive_sha256") == csv_sha
        and provenance.get("lane_result_sha256") == result_sha == checked_result_sha
        and provenance.get("run_id") == manifest.get("run_id")
        and provenance.get("mode") == manifest.get("mode") == "managed"
        and provenance.get("sut_pid") == manifest.get("pid")
        and provenance.get("sut_start_time_ticks") == manifest.get("pid_start_time_ticks")
        and provenance.get("lane") == manifest.get("comparison_lane")
        and provenance.get("graceful_finalization") is True
        and provenance.get("lane_result_identity_verified") is True)
