import json
import os
from pathlib import Path

import pytest

from mw_perf_collect.adapters.ibmw import archive_core_metrics, binary_identity, collect_ibmw, core_metrics_snapshot, source_identity, validate_core_metrics


def test_collects_version_cmake_transport_and_degrades_core(tmp_path: Path):
    source = tmp_path / "IBMW"
    (source / "src/api/cpp/util").mkdir(parents=True)
    (source / "src/api/cpp/util/version.h").write_text('#define IBMW_RUNTIME_VERSION "1.2.3"\n')
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("IBMW_BUILD_RUNTIME:BOOL=ON\nIBMW_BUILD_NET_EXTENSION:BOOL=OFF\n")
    transport = tmp_path / "transport.yaml"
    transport.write_text("transport: dds\nshm_enabled: false\nother: hidden\n")

    result = collect_ibmw(source, build_dir=build, transport_config=transport)

    assert result["schema_version"] == "mw_perf_collect.v1"
    assert result["manifest"]["version"]["value"] == "1.2.3"
    assert result["manifest"]["build"]["options"]["IBMW_BUILD_RUNTIME:BOOL"] == "ON"
    assert result["topology"]["transport"]["summary"] == {"transport": "dds", "shm_enabled": False}
    assert result["capability"]["core_metrics"]["status"] == "unsupported"
    assert result["capability"]["degradations"]


def test_reads_optional_core_capability_json(tmp_path: Path):
    capability = tmp_path / "core-capability.json"
    capability.write_text(json.dumps({"mode": "counters", "version": 1}))
    result = collect_ibmw(tmp_path, core_capability=capability, cmake_executable="mock-cmake")
    assert result["capability"]["core_metrics"]["value"]["mode"] == "counters"
    assert result["capability"]["degradations"] == []


def test_binary_identity_resolves_and_hashes(tmp_path: Path):
    binary = tmp_path/"real"; binary.write_bytes(b"abc")
    link = tmp_path/"link"; link.symlink_to(binary)
    value = binary_identity(link)
    assert value["resolved_path"] == str(binary)
    assert value["device"] == binary.stat().st_dev
    assert value["inode"] == binary.stat().st_ino
    assert value["size"] == 3 and value["sha256"] == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


def test_binary_identity_rejects_path_replacement_during_fd_hash(tmp_path: Path, monkeypatch):
    binary = tmp_path / "sut"
    original = b"old" * (1024 * 1024)
    replacement_content = b"new" * (1024 * 1024)
    binary.write_bytes(original)
    old_info = binary.stat()
    replacement = tmp_path / "replacement"
    replacement.write_bytes(replacement_content)
    real_stat = Path.stat
    stat_calls = 0
    replaced = False

    def replacing_stat(path, *args, **kwargs):
        nonlocal replaced, stat_calls
        info = real_stat(path, *args, **kwargs)
        stat_calls += 1
        if stat_calls == 2:
            replacement.replace(binary)
            replaced = True
        return info

    monkeypatch.setattr(Path, "stat", replacing_stat)
    with pytest.raises(OSError, match="artifact (?:path )?changed while hashing"):
        binary_identity(binary)

    assert replaced
    assert real_stat(binary).st_ino != old_info.st_ino
    assert binary.read_bytes() == replacement_content


def test_required_provenance_needs_explicit_artifact_list(tmp_path: Path):
    source = tmp_path/"source"; source.mkdir()
    build = tmp_path/"build"; build.mkdir(); (build/"CMakeCache.txt").write_text("X:BOOL=ON\n")
    executable = tmp_path/"sut"; executable.write_bytes(b"x")
    try:
        collect_ibmw(source, build_dir=build, provenance={"required": True, "sut_executable": str(executable)})
    except ValueError as exc:
        assert "artifact_binaries" in str(exc)
    else:
        raise AssertionError("required provenance accepted an implicit wrapper-only artifact")


def test_source_identity_binds_untracked_content(tmp_path: Path):
    import subprocess
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.email", "test@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.name", "Test"], check=True)
    tracked = tmp_path/"tracked"; tracked.write_text("tracked\n")
    subprocess.run(["git", "-C", str(tmp_path), "add", "tracked"], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "commit", "-qm", "initial"], check=True)
    untracked = tmp_path/"new-file"; untracked.write_text("one")
    first = source_identity(tmp_path)
    untracked.write_text("two")
    second = source_identity(tmp_path)
    assert first["untracked_count"] == second["untracked_count"] == 1
    assert first["untracked_tree_sha256"] != second["untracked_tree_sha256"]
    assert first["working_tree_sha256"] != second["working_tree_sha256"]


def _record(seq=1, kind="latency"):
    return {"schema": "ibmw.core_metrics.v1", "snapshot_seq": seq, "kind": kind,
            "cumulative": {"count": seq}, "complete": True}


def test_core_metrics_validator_accepts_multiple_and_unknown_kinds(tmp_path: Path):
    path = tmp_path / "metrics.jsonl"
    path.write_text("\n".join(json.dumps(_record(seq, kind)) for seq, kind in ((1, "latency"), (2, "future_kind"))) + "\n")
    result = validate_core_metrics(path)
    assert result == {"valid": True, "record_count": 2, "kinds": ["future_kind", "latency"], "validation_errors": []}


def test_core_metrics_validator_rejects_contract_errors(tmp_path: Path):
    cases = {
        "truncated": json.dumps(_record()),
        "empty-line": json.dumps(_record()) + "\n\n",
        "schema": json.dumps({**_record(), "schema": "wrong"}) + "\n",
        "duplicate": json.dumps(_record()) + "\n" + json.dumps(_record()) + "\n",
    }
    for name, content in cases.items():
        path = tmp_path / f"{name}.jsonl"; path.write_text(content)
        assert validate_core_metrics(path)["valid"] is False, name


def test_core_metrics_validator_rejects_empty_and_truncated_jsonl(tmp_path: Path):
    for name, content in (("zero_bytes", b""), ("truncated", json.dumps(_record()).encode())):
        path = tmp_path / f"{name}.jsonl"
        path.write_bytes(content)
        result = validate_core_metrics(path)
        assert result["valid"] is False
        assert result["record_count"] == (0 if name == "zero_bytes" else 1)
        if name == "zero_bytes":
            assert result["validation_errors"] == ["file is empty"]


def test_core_metrics_archive_detects_stale_and_missing_optional(tmp_path: Path):
    directory = tmp_path / "source"; directory.mkdir()
    missing = archive_core_metrics({"directory": directory}, core_metrics_snapshot({"directory": directory}), tmp_path/"missing.jsonl", managed=False)
    assert missing["status"] == "unsupported" and missing["required"] is False
    source = directory / "metrics.jsonl"; source.write_text(json.dumps(_record()) + "\n")
    baseline = core_metrics_snapshot({"directory": directory})
    stale = archive_core_metrics({"directory": directory}, baseline, tmp_path/"archive.jsonl", managed=True)
    assert stale["status"] == "stale"


def test_core_metrics_staleness_uses_content_not_metadata(tmp_path: Path):
    directory = tmp_path / "source"; directory.mkdir()
    source = directory / "metrics.jsonl"; source.write_text(json.dumps(_record()) + "\n")
    baseline = core_metrics_snapshot({"directory": directory})
    os.utime(source, ns=(baseline["mtime_ns"] + 1, baseline["mtime_ns"] + 1))
    assert archive_core_metrics({"directory": directory}, baseline,
                                tmp_path/"stale.jsonl", managed=True)["status"] == "stale"

    replacement = json.dumps(_record(kind="network")) + "\n"
    assert len(replacement.encode()) == source.stat().st_size
    source.write_text(replacement)
    changed = archive_core_metrics({"directory": directory}, baseline,
                                   tmp_path/"changed.jsonl", managed=True, run_id="run-1",
                                   build={"configured": True}, root=str(tmp_path))
    assert changed["status"] == "supported"
    assert changed["content_sha256"] != baseline["content_sha256"]
    provenance = changed["provenance"]
    assert provenance["source_sha256"] == provenance["archive_sha256"] == changed["content_sha256"]
    assert provenance["run_id"] == "run-1"
    assert provenance["baseline_metadata"]["size"] == baseline["size"]
    assert provenance["final_metadata"]["size"] == source.stat().st_size


def test_stale_archive_preserves_provenance_diagnostics(tmp_path: Path):
    directory = tmp_path / "source"; directory.mkdir()
    source = directory / "metrics.jsonl"; source.write_text(json.dumps(_record()) + "\n")
    baseline = core_metrics_snapshot({"directory": directory})
    result = archive_core_metrics({"directory": directory}, baseline, tmp_path/"archive.jsonl",
                                  managed=True, run_id="stale-run", build={}, root=str(directory))
    assert result["status"] == "stale"
    assert result["provenance"]["source_sha256"] == baseline["content_sha256"]
    assert result["provenance"]["archive_sha256"] is None


def test_attached_required_fails_closed_but_optional_is_diagnostic(tmp_path: Path):
    directory = tmp_path / "source"; directory.mkdir()
    baseline = core_metrics_snapshot({"directory": directory})
    (directory / "metrics.jsonl").write_text(json.dumps(_record()) + "\n")
    required = archive_core_metrics({"directory": directory, "required": True}, baseline,
                                    tmp_path/"required.jsonl", managed=False, mode="attached")
    optional = archive_core_metrics({"directory": directory}, baseline,
                                    tmp_path/"optional.jsonl", managed=False, mode="attached")
    assert required["status"] == "invalid"
    assert optional["status"] == "partial"
    assert required["provenance"]["final_snapshot"] is False
    assert optional["provenance"]["final_snapshot"] is False
    assert optional["archive_path"] == str(tmp_path/"optional.jsonl")


def test_managed_archive_returns_complete_provenance_closure(tmp_path: Path):
    directory = tmp_path / "source"
    directory.mkdir()
    source = directory / "metrics.jsonl"
    source.write_text(json.dumps(_record()) + "\n")
    baseline = core_metrics_snapshot({"directory": directory, "required": True})
    source.write_text(json.dumps(_record(kind="latency")) + "\n" +
                      json.dumps(_record(seq=2, kind="future_kind")) + "\n")
    archive = tmp_path / "raw" / "core_metrics.jsonl"
    result = archive_core_metrics({"directory": directory, "required": True}, baseline, archive,
                                  managed=True, run_id="run", adapter="ibmw", build={},
                                  root="/source", mode="managed", sut_pid=123,
                                  sut_start_time_ticks=456)
    assert result["status"] == "supported"
    assert result["record_count"] == 2
    assert result["kinds"] == ["future_kind", "latency"]
    digest = result["content_sha256"]
    assert result["provenance"]["source_sha256"] == digest
    assert result["provenance"]["archive_sha256"] == digest
    assert result["provenance"]["graceful_finalization"] is True
    assert result["provenance"]["final_snapshot"] is True
