import copy, json, os, signal
from pathlib import Path
from typing import Any, cast

import mw_perf_collect.core as core
import mw_perf_collect.adapters as adapters
import pytest
from mw_perf_collect.core import proc_stat, validate_dir, build_p0a_evidence_manifest, validate_p0a_evidence_manifest


_P0A_VERSION = "mw_perf.p0a_contract.v1"


def _p0a_record(schema, **fields):
    return {"schema": schema, "run_id": "r", "capability_id": "c", "path_id": "p",
            "deployment_mode": "managed", "finalized": True, **fields}


def _write_p0a_artifacts(path, run_type="p0a_proof", observer=False):
    proof = {
        "capability/resolution.json": _p0a_record("mw_perf.p0a.resolution.v1", resolved=True, conclusion="pass"),
        "capability/hook_callgraph.json": _p0a_record("mw_perf.p0a.hook_callgraph.v1", hooks_verified=True, conclusion="pass"),
        "normalized/identity_trace.jsonl": _p0a_record("mw_perf.p0a.identity_trace.v1", identity_verified=True, conclusion="pass"),
        "validation/fault_injection.json": _p0a_record("mw_perf.p0a.fault_injection.v1", fault_detected=True, conclusion="pass"),
        "validation/rollback.json": _p0a_record("mw_perf.p0a.rollback.v1", rollback_succeeded=True, health_check_passed=True, conclusion="pass"),
    }
    collection = {
        "normalized/comm_events.jsonl": _p0a_record("mw_perf.comm_event.v1"),
        "normalized/latency_samples.jsonl": _p0a_record("mw_perf.latency_sample.v1"),
        "capability/communication_latency.json": _p0a_record("mw_perf.communication_latency.v1", conclusion="pass"),
        "validation/communication_delivery.json": _p0a_record("mw_perf.communication_delivery.v1", conclusion="pass"),
    }
    selected = {}
    if run_type in ("p0a_proof", "exact_run"): selected.update(proof)
    if run_type in ("stage_run", "exact_run", "observer_diagnostic"):
        selected.update(collection)
        trace = "raw/comm_trace/nested/events.jsonl"
        selected[trace] = _p0a_record("mw_perf.comm_trace.v1")
        selected["raw/comm_trace/directory_finalization.json"] = _p0a_record(
            "mw_perf.comm_trace.finalization.v1", files=[trace])
    if observer: selected["capability/observer_isolation.json"] = _p0a_record(
        "mw_perf.p0a.observer_isolation.v1", isolated=True, conclusion="pass")
    for relative, value in selected.items():
        target = path / relative; target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(value) + ("\n" if relative.endswith(".jsonl") else ""))


def _build_p0a(path, run_type="p0a_proof", observer=False, measurement_kind="exact_correlated"):
    _write_p0a_artifacts(path, run_type, observer)
    manifest = build_p0a_evidence_manifest(path, {"registration_contract_version": _P0A_VERSION},
        run_id="r", capability_id="c", path_id="p", deployment_mode="managed", run_type=run_type,
        observer=observer, measurement_kind=measurement_kind)
    assert manifest is not None
    return manifest


def test_p0a_uses_trusted_contract_and_fixed_universes(tmp_path):
    proof = _build_p0a(tmp_path / "proof")
    stage = _build_p0a(tmp_path / "stage", "stage_run")
    exact = _build_p0a(tmp_path / "exact", "exact_run")
    assert validate_p0a_evidence_manifest(tmp_path / "proof", proof)["valid"]
    assert {x["relative_path"] for x in proof["required"]} >= {
        "capability/resolution.json", "capability/hook_callgraph.json", "normalized/identity_trace.jsonl",
        "validation/fault_injection.json", "validation/rollback.json"}
    assert "capability/resolution.json" not in {x["relative_path"] for x in stage["entries"]}
    assert "capability/resolution.json" in {x["relative_path"] for x in exact["entries"]}
    assert "normalized/comm_events.jsonl" in {x["relative_path"] for x in exact["entries"]}
    trusted = (Path(core.__file__).parent / "contracts/p0a-evidence-contract.v1.json").read_bytes()
    assert (tmp_path / "proof/registration/contract.json").read_bytes() == trusted


def test_trusted_contract_v1_through_v5_bytes_are_immutable():
    contracts = Path(core.__file__).parent / "contracts"
    expected = {
        "p0a-evidence-contract.v1.json": "9bc17677e794d4341656b16af66ff121211e3bd3d4dba8d162a889c41a3e28eb",
        "p0a-evidence-contract.v2.json": "dc2da2f476bbffe4e0728455279e8841b00ae01b3bf57612f3e84aa7dedcddd5",
        "p0a-evidence-contract.v3.json": "e122d6b8dd774c5c2a458f5557624cc5fa49c9d259e9b2a604344310020c2c9d",
        "p0a-evidence-contract.v4.json": "99ba12db651bec747d9d03b46ae48a84db3015bd4e485852448c49d82b164cf2",
        "p0a-evidence-contract.v5.json": "d564dbaf47ac11101c691c66168d5708025640b563d96378c0a94e43fb8d7004",
    }
    for name, digest in expected.items():
        assert core.hashlib.sha256((contracts / name).read_bytes()).hexdigest() == digest


def test_p0a_rejects_minimal_or_modified_contract(tmp_path):
    fake = {"registration_contract_version": _P0A_VERSION, "artifacts": []}
    assert build_p0a_evidence_manifest(tmp_path, {"registration_contract": fake}, run_id="r",
        capability_id="c", path_id="p", deployment_mode="managed", run_type="p0a_proof",
        observer=False, measurement_kind="exact_correlated") is None
    manifest = _build_p0a(tmp_path / "valid")
    (tmp_path / "valid/registration/contract.json").write_text("{}")
    assert "P0A_CONTRACT_HASH" in validate_p0a_evidence_manifest(tmp_path / "valid", manifest)["failure_codes"]


def test_p0a_rejects_status_set_order_schema_and_hash_tampering(tmp_path):
    path = tmp_path / "run"; manifest = _build_p0a(path)
    mutations = (
        ("status", lambda value: value["entries"][0].update(status="not_applicable"), "P0A_STATUS"),
        ("order", lambda value: value["entries"].reverse(), "P0A_ENTRY_ORDER"),
        ("extra", lambda value: value["entries"].append(dict(value["entries"][0], relative_path="extra.json")), "P0A_ENTRY_SET"),
        ("schema", lambda value: value["entries"][0].update(schema_version="wrong.v1"), "P0A_SCHEMA_VERSION"),
        ("hash", lambda value: value["entries"][0].update(sha256="0" * 64), "P0A_ARTIFACT"),
    )
    for _name, mutate, code in mutations:
        changed = copy.deepcopy(manifest); mutate(changed)
        assert code in validate_p0a_evidence_manifest(path, changed)["failure_codes"]


def test_p0a_rejects_schema_identity_finalization_conclusion_and_nonfinite_json(tmp_path):
    cases = (
        ("schema", "capability/resolution.json", {"schema": "wrong"}),
        ("identity", "capability/resolution.json", {"run_id": "other"}),
        ("finalization", "capability/resolution.json", {"finalized": False}),
        ("conclusion", "validation/rollback.json", {"conclusion": "fail"}),
    )
    for name, relative, mutation in cases:
        path = tmp_path / name; manifest = _build_p0a(path)
        artifact = path / relative; value = json.loads(artifact.read_text()); value.update(mutation)
        artifact.write_text(json.dumps(value))
        assert "P0A_ARTIFACT" in validate_p0a_evidence_manifest(path, manifest)["failure_codes"], name
    path = tmp_path / "nan"; manifest = _build_p0a(path)
    (path / "normalized/identity_trace.jsonl").write_text('{"schema":"mw_perf.p0a.identity_trace.v1","run_id":"r","capability_id":"c","path_id":"p","deployment_mode":"managed","finalized":true,"identity_verified":true,"conclusion":"pass","value":NaN}\n')
    assert "P0A_ARTIFACT" in validate_p0a_evidence_manifest(path, manifest)["failure_codes"]


def test_p0a_min_zero_allows_real_empty_jsonl_and_rejects_symlink_or_hardlink(tmp_path):
    empty = tmp_path / "empty"; manifest = _build_p0a(empty, "stage_run")
    artifact = empty / "normalized/comm_events.jsonl"; artifact.write_bytes(b"")
    entry = next(item for item in manifest["entries"] if item["relative_path"] == "normalized/comm_events.jsonl")
    entry.update(sha256=core.hashlib.sha256(b"").hexdigest(), record_count=0)
    manifest["required"] = [item for item in manifest["entries"] if item["status"] == "required"]
    assert validate_p0a_evidence_manifest(empty, manifest)["valid"]
    symlink = tmp_path / "symlink"; symlink_manifest = _build_p0a(symlink)
    target = symlink / "capability/resolution.json"; original = target.read_bytes(); target.unlink()
    outside = tmp_path / "outside.json"; outside.write_bytes(original); target.symlink_to(outside)
    assert "P0A_ARTIFACT" in validate_p0a_evidence_manifest(symlink, symlink_manifest)["failure_codes"]
    hardlink = tmp_path / "hardlink"; hardlink_manifest = _build_p0a(hardlink)
    identity = hardlink / "normalized/identity_trace.jsonl"; identity.unlink()
    os.link(hardlink / "capability/resolution.json", identity)
    assert "P0A_ARTIFACT" in validate_p0a_evidence_manifest(hardlink, hardlink_manifest)["failure_codes"]


def test_p0a_rejects_symlinked_contract_archive_and_snapshot_replacement(tmp_path, monkeypatch):
    archive_case = tmp_path / "archive"; archive_case.mkdir()
    outside = tmp_path / "outside"; outside.mkdir()
    (archive_case / "registration").symlink_to(outside, target_is_directory=True)
    try:
        build_p0a_evidence_manifest(archive_case, {"registration_contract_version": _P0A_VERSION},
            run_id="r", capability_id="c", path_id="p", deployment_mode="managed",
            run_type="p0a_proof", observer=False, measurement_kind="exact_correlated")
    except OSError:
        pass
    else:
        raise AssertionError("symlinked registration archive was accepted")
    snapshot_case = tmp_path / "snapshot"; manifest = _build_p0a(snapshot_case)
    original_stat = core._secure_stat
    monkeypatch.setattr(core, "_secure_stat", lambda path, relative: (
        (*original_stat(path, relative)[:2], original_stat(path, relative)[2] + 1,
         original_stat(path, relative)[3]) if relative == "capability/resolution.json"
        else original_stat(path, relative)))
    assert "P0A_SNAPSHOT_CHANGED" in validate_p0a_evidence_manifest(snapshot_case, manifest)["failure_codes"]


def test_p0a_comm_trace_prefix_is_directory_exact(tmp_path):
    manifest = _build_p0a(tmp_path, "stage_run")
    (tmp_path / "raw/comm_trace_backup.jsonl").write_text("not part of trace directory")
    assert validate_p0a_evidence_manifest(tmp_path, manifest)["valid"]


def test_p0a_final_trace_revalidation_receives_collector_controls(tmp_path, monkeypatch):
    manifest = _build_p0a(tmp_path, "exact_run")
    captured = {}
    trusted_contract = core._trusted_p0a_contract
    decode = core._p0a_decode

    def exact_pattern_contract(version):
        contract, raw = trusted_contract(version)
        contract = copy.deepcopy(contract)
        contract["registration_contract_version"] = "mw_perf.p0a_contract.v2"
        directory = next(item for item in contract["artifacts"]
                         if item["relative_path"] == "raw/comm_trace/")
        directory["format"] = "pattern_directory"
        return contract, raw

    def decode_pattern(data, declared):
        if declared.get("format") == "pattern_directory":
            declared = {**declared, "format": "jsonl", "record_count_rule": "physical_lines"}
        return decode(data, declared)

    def validate_directory_files(*args, **kwargs):
        captured.update(kwargs)
        return {"status": "supported"}

    import mw_perf_collect.dds_trace as dds_trace
    monkeypatch.setattr(core, "_trusted_p0a_contract", exact_pattern_contract)
    monkeypatch.setattr(core, "_p0a_decode", decode_pattern)
    monkeypatch.setattr(dds_trace, "validate_directory_files", validate_directory_files)
    result = validate_p0a_evidence_manifest(tmp_path, manifest,
        producer_pid=123, producer_start_time_ticks=456,
        topology_generation="generation-1", topology_sha256="ab" * 32)
    assert result["valid"]
    assert result["dds_comm_trace"] == {"required": True, "status": "supported"}
    assert {name: captured[name] for name in (
        "producer_pid", "producer_start_time_ticks", "topology_generation", "topology_sha256")} == {
            "producer_pid": 123, "producer_start_time_ticks": 456,
            "topology_generation": "generation-1", "topology_sha256": "ab" * 32}
    for controls in (
        {"producer_pid": 123, "producer_start_time_ticks": 456,
         "topology_generation": None, "topology_sha256": "ab" * 32},
        {"producer_pid": True, "producer_start_time_ticks": 456,
         "topology_generation": "generation-1", "topology_sha256": "ab" * 32},
    ):
        rejected = validate_p0a_evidence_manifest(tmp_path, manifest, **controls)
        assert "P0A_DDS_COMM_TRACE" in rejected["failure_codes"]


def test_p0a_exact_rejects_nested_contract_downgrade_and_top_level_mismatches(tmp_path):
    manifest = _build_p0a(tmp_path, "exact_run")
    expected = {
        "run_id": "r", "registration_contract_version": "mw_perf.p0a_contract.v2",
        "run_type": "exact_run", "observer": False, "capability_id": "c", "path_id": "p",
        "deployment_mode": "managed", "measurement_kind": "exact_correlated",
    }
    cases = (
        ("v1", {}),
        ("stage", {"registration_contract_version": "mw_perf.p0a_contract.v2",
                   "run_type": "stage_run"}),
        ("identity", {"registration_contract_version": "mw_perf.p0a_contract.v2",
                      "capability_id": "other"}),
        ("measurement", {"registration_contract_version": "mw_perf.p0a_contract.v2",
                         "measurement_kind": "stage_only"}),
    )
    for name, changes in cases:
        nested = copy.deepcopy(manifest)
        nested.update(changes)
        result = validate_p0a_evidence_manifest(
            tmp_path, nested, expected_exact_metadata=expected)
        assert result["failure_codes"] == ["P0A_TOP_LEVEL_BINDING"], name


def test_validate_dir_sources_final_trace_controls_from_trusted_manifest_fields(tmp_path, monkeypatch):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest.update(validation_profile="p0a", pid=123, pid_start_time_ticks=456,
        dds_comm_trace={"topology_generation": "generation-1", "topology_sha256": "ab" * 32})
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))
    captured = {}

    def validate_manifest(_path, **kwargs):
        captured.update(kwargs)
        return {"valid": True, "failure_codes": []}

    monkeypatch.setattr(core, "validate_p0a_evidence_manifest", validate_manifest)
    validate_dir(tmp_path, profile="p0a")
    assert captured == {"producer_pid": 123, "producer_start_time_ticks": 456,
        "topology_generation": "generation-1", "topology_sha256": "ab" * 32,
        "expected_exact_metadata": None}


def test_validate_dir_reconciles_exact_dds_trace_summary(tmp_path, monkeypatch):
    expected_metadata = {
        "registration_contract_version": "mw_perf.p0a_contract.v2",
        "run_type": "exact_run", "observer": False,
        "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "path_id": "default", "deployment_mode": "managed",
        "measurement_kind": "exact_correlated",
    }
    fresh = {"required": True, "status": "exact_correlated", "runtime_changed": True,
        "business_unchanged": True, "record_count": 2, "exact_record_count": 1,
        "stream_count": 1,
        "segment_count": 1, "dropped": 0, "invalid_identity": 0,
        "missing_registration": 0, "unsupported_async": 0,
        "unsupported_multi_handler": 0, **expected_metadata,
        "run_id": "test-run", "topology_generation": "generation-1",
        "topology_sha256": "ab" * 32}

    def prepare(path):
        _write_valid_artifacts(path)
        manifest = json.loads((path / "manifest.json").read_text())
        manifest.update(validation_profile="p0a", run_type="exact_run",
                        measurement_kind="exact_correlated")
        fresh["run_id"] = manifest["run_id"]
        manifest["dds_comm_trace"] = {key: value for key, value in fresh.items()
                                      if key != "run_id"}
        (path / "manifest.json").write_text(json.dumps(manifest))

    monkeypatch.setattr(core, "validate_p0a_evidence_manifest", lambda *_args, **_kwargs: {
        "valid": True, "failure_codes": [], "dds_comm_trace": copy.deepcopy(fresh)})
    success = tmp_path / "success"; success.mkdir(); prepare(success)
    assert validate_dir(success, profile="p0a")["valid_for_comparison"]

    for name, mutate in (
        ("missing_required", lambda trace: trace.pop("required")),
        ("status", lambda trace: trace.update(status="supported")),
        ("counter", lambda trace: trace.update(missing_registration=1)),
        ("count", lambda trace: trace.update(record_count=3)),
        ("exact_count", lambda trace: trace.update(exact_record_count=3)),
        ("missing_exact_count", lambda trace: trace.pop("exact_record_count")),
    ):
        path = tmp_path / name; path.mkdir(); prepare(path)
        manifest = json.loads((path / "manifest.json").read_text())
        mutate(manifest["dds_comm_trace"])
        (path / "manifest.json").write_text(json.dumps(manifest))
        result = validate_dir(path, profile="p0a")
        assert "DDS_COMM_TRACE_SUMMARY" in result["failure_codes"], name


def test_validate_dir_profile_is_external_and_p0a_fails_closed(tmp_path):
    _write_valid_artifacts(tmp_path)
    assert validate_dir(tmp_path, profile="standard")["valid_for_comparison"]
    result = validate_dir(tmp_path, profile="p0a")
    assert not result["valid_for_comparison"]
    assert "P0A_PROFILE_MARKER" in result["failure_codes"]
    assert result["p0a_evidence"]["failure_codes"] == ["P0A_MANIFEST_MISSING"]
    manifest = json.loads((tmp_path / "manifest.json").read_text()); manifest["validation_profile"] = "p0a"
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))
    assert not validate_dir(tmp_path, profile="p0a")["valid_for_comparison"]


def test_ros2_adapter_only_uses_whitelisted_sut_environment(tmp_path, monkeypatch):
    captured = {}

    class FakeRos2Adapter:
        SUT_ENV_ALLOWLIST = adapters.Ros2Adapter.SUT_ENV_ALLOWLIST
        UNSAFE_ENV_KEYS = adapters.Ros2Adapter.UNSAFE_ENV_KEYS

        def __init__(self, *, environ, ros2_executable=None, ros2_prefix=None):
            captured.update(environ)
            captured["ros2_executable"] = ros2_executable
            captured["ros2_prefix"] = ros2_prefix

        def run(self, options):
            return {"status": "ok", "readiness": {"ok": True}, "observations": []}

    monkeypatch.setattr(adapters, "Ros2Adapter", FakeRos2Adapter)
    for directory in ("environment", "topology", "capability"):
        (tmp_path / directory).mkdir()
    result = core._collect_adapter(
        "ros2",
        {"sut": {"env": {"ROS_DOMAIN_ID": "45", "PYTHONPATH": "/sut/plugin",
                          "LD_PRELOAD": "/sut/hook.so", "PATH": "/sut/bin"}},
         "adapters": {"ros2": {}}},
        tmp_path,
        123,
    )
    assert result["status"] == "ok"
    assert captured["ROS_DOMAIN_ID"] == "45"
    assert "PYTHONPATH" not in captured
    assert "LD_PRELOAD" not in captured
    assert captured["PATH"] != "/sut/bin"


def test_core_forwards_ros2_prefix_for_doctor_and_run(tmp_path, monkeypatch):
    captured = []

    class FakeRos2Adapter:
        SUT_ENV_ALLOWLIST = adapters.Ros2Adapter.SUT_ENV_ALLOWLIST

        @classmethod
        def sanitized_environment(cls, source, *, include_ros=True):
            return adapters.Ros2Adapter.sanitized_environment(source, include_ros=include_ros)

        @classmethod
        def valid_environment_value(cls, key, value):
            return adapters.Ros2Adapter.valid_environment_value(key, value)

        def __init__(self, **kwargs):
            captured.append(kwargs)

        def doctor(self):
            return {"adapter": "ros2", "status": "ready"}

        def run(self, options):
            return {"status": "ok", "readiness": {"ok": True}, "observations": []}

    monkeypatch.setattr(adapters, "Ros2Adapter", FakeRos2Adapter)
    config = {"sut": {"env": {}}, "adapters": {"ros2": {
        "ros2_prefix": "/opt/ros/humble", "ros2_executable": "/opt/ros/humble/bin/ros2"}}}
    assert core.adapter_doctor("ros2", config)["status"] == "ready"
    for directory in ("environment", "topology", "capability"):
        (tmp_path / directory).mkdir()
    assert core._collect_adapter("ros2", config, tmp_path, 123)["status"] == "ok"
    assert [item["ros2_prefix"] for item in captured] == [
        "/opt/ros/humble", "/opt/ros/humble"]
    assert all(item["ros2_executable"] == "/opt/ros/humble/bin/ros2"
               for item in captured)


def test_proc_stat_reads_pss_from_smaps_rollup_not_status(monkeypatch):
    stat = ["0"] * 24
    stat[13], stat[14], stat[21], stat[23] = "10", "5", "99", "2"

    def fake_read(path, *args, **kwargs):
        value = str(path)
        if value.endswith("/stat"):
            return " ".join(stat)
        if value.endswith("/status"):
            return "Threads:\t3\nPss:\t999 kB\n"
        if value.endswith("/smaps_rollup"):
            return "Rss: 20 kB\nPss: 7 kB\n"
        raise AssertionError(value)

    monkeypatch.setattr(Path, "read_text", fake_read)
    result = proc_stat(42)
    assert result is not None
    assert result["pss_bytes"] == 7 * 1024
    assert result["rss_bytes"] == 2 * os.sysconf("SC_PAGE_SIZE")


def test_proc_stat_reports_unavailable_pss_when_rollup_unreadable(monkeypatch):
    stat = ["0"] * 24
    stat[13], stat[14], stat[21], stat[23] = "1", "1", "1", "1"

    def fake_read(path, *args, **kwargs):
        if str(path).endswith("/stat"):
            return " ".join(stat)
        if str(path).endswith("/status"):
            return "Threads: 1\nPss: 999 kB\n"
        raise PermissionError

    monkeypatch.setattr(Path, "read_text", fake_read)
    result = proc_stat(42)
    assert result is not None
    assert result["pss_bytes"] is None


def test_proc_stat_parses_comm_with_spaces_and_parentheses(monkeypatch):
    fields = ["S"] + ["0"] * 51
    fields[11], fields[12], fields[19], fields[21] = "10", "5", "99", "2"
    stat = "42 (worker name (aux)) " + " ".join(fields)

    def fake_read(path, *args, **kwargs):
        value = str(path)
        if value.endswith("/stat"):
            return stat
        if value.endswith("/status"):
            return "Threads: 3\n"
        if value.endswith("/smaps_rollup"):
            return "Pss: 7 kB\n"
        raise AssertionError(value)

    monkeypatch.setattr(Path, "read_text", fake_read)
    result = proc_stat(42)
    assert result is not None
    assert result["cpu_seconds"] == 15 / os.sysconf("SC_CLK_TCK")
    assert result["start_time_ticks"] == 99


def test_validate_dir_fails_closed_for_missing_required_field(tmp_path):
    manifest: dict[str, object] = {name: True for name in (
        "schema_version", "tool_version", "run_id", "adapter", "mode", "run_status",
        "scenario_contract_hash", "metric_availability", "measurement_kind", "start_time",
        "end_time", "pid", "states", "sample_count", "metrics_valid", "adapter_readiness",
    )}
    manifest.update(run_status="completed", sample_count=1, metric_availability="supported")
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    (tmp_path/"raw").mkdir()
    (tmp_path/"raw/process.jsonl").write_text("{}\n")
    (tmp_path/"raw/network.jsonl").write_text("{}\n")
    result = validate_dir(tmp_path)
    assert result["valid_for_collection"] is False
    assert "REQUIRED_FIELD_MISSING:pid_start_time_ticks" in result["failure_codes"]


def _valid_manifest():
    return {
        "schema_version": 1,
        "tool_version": "0.1.0-mvp",
        "run_id": "test-run",
        "adapter": "generic",
        "mode": "managed",
        "run_status": "completed",
        "scenario_contract_hash": "0" * 64,
        "metric_availability": "supported",
        "measurement_kind": "metadata",
        "start_time": 1.0,
        "end_time": 2.0,
        "pid": 1,
        "pid_start_time_ticks": 2,
        "states": ["PREFLIGHT", "PACKAGE"],
        "sample_count": 1,
        "metrics_valid": True,
        "adapter_readiness": True,
    }


def _write_valid_artifacts(path):
    (path/"raw").mkdir()
    process = {"timestamp": 1.0, "processes": [{"pid": 1, "cpu_seconds": 0.1,
        "rss_bytes": 10, "pss_bytes": None, "threads": 1, "start_time_ticks": 2}]}
    network = {"timestamp": 1.0, "interfaces": {"eth0": {"rx_bytes": 1,
        "tx_bytes": 2, "rx_packets": 3, "tx_packets": 4}}}
    (path/"raw/process.jsonl").write_text(json.dumps(process) + "\n")
    (path/"raw/network.jsonl").write_text(json.dumps(network) + "\n")
    manifest = _valid_manifest()
    manifest["sample_artifacts"] = core._sample_artifact_evidence(path)
    (path/"manifest.json").write_text(json.dumps(manifest))


def test_validate_dir_accepts_bound_sample_artifacts(tmp_path):
    _write_valid_artifacts(tmp_path)
    result = validate_dir(tmp_path)
    assert result["valid_for_collection"] is True
    assert result["valid_for_comparison"] is True


def test_validate_dir_rejects_process_and_network_numeric_tampering(tmp_path):
    for name, field in (("process", "rss_bytes"), ("network", "rx_bytes")):
        case = tmp_path/name
        case.mkdir()
        _write_valid_artifacts(case)
        artifact = case/f"raw/{name}.jsonl"
        record = json.loads(artifact.read_text())
        if name == "process":
            record["processes"][0][field] += 1
        else:
            record["interfaces"]["eth0"][field] += 1
        artifact.write_text(json.dumps(record) + "\n")
        assert validate_dir(case)["valid_for_collection"] is False, name


def test_validate_dir_rejects_added_and_removed_sample_records(tmp_path):
    for mutation in ("add", "remove"):
        case = tmp_path/mutation
        case.mkdir()
        _write_valid_artifacts(case)
        artifact = case/"raw/process.jsonl"
        if mutation == "add":
            artifact.write_text(artifact.read_text() * 2)
        else:
            artifact.write_text("")
        assert validate_dir(case)["valid_for_collection"] is False, mutation


def test_validate_dir_rejects_missing_sample_artifact_digest(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["sample_artifacts"]["process"].pop("sha256")
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    assert validate_dir(tmp_path)["valid_for_collection"] is False


def test_validate_dir_rejects_corrupt_jsonl_line(tmp_path):
    _write_valid_artifacts(tmp_path)
    with (tmp_path/"raw/process.jsonl").open("a") as stream:
        stream.write("{broken\n")
    result = validate_dir(tmp_path)
    assert result["valid_for_collection"] is False
    assert "SAMPLE_ARTIFACTS" in result["failure_codes"]


def test_validate_dir_rejects_invalid_process_and_network_records(tmp_path):
    _write_valid_artifacts(tmp_path)
    (tmp_path/"raw/process.jsonl").write_text('{"timestamp": 1, "processes": []}\n')
    (tmp_path/"raw/network.jsonl").write_text('{"timestamp": 1, "interfaces": []}\n')
    assert validate_dir(tmp_path)["valid_for_collection"] is False


def test_validate_dir_rejects_bool_and_wrong_manifest_types(tmp_path):
    for field, value in (("schema_version", True), ("pid", True),
                         ("sample_count", True), ("states", "PACKAGE")):
        case = tmp_path / field
        case.mkdir()
        _write_valid_artifacts(case)
        manifest = json.loads((case / "manifest.json").read_text())
        manifest[field] = value
        (case / "manifest.json").write_text(json.dumps(manifest))
        result = validate_dir(case)
        assert result["valid_for_collection"] is False, field
        assert f"MANIFEST_SCHEMA:{field}" in result["failure_codes"]


def test_runtime_image_evidence_rejects_non_object_and_empty_matches(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest["adapter"] = "ibmw"
    manifest["provenance"] = {"required": True}
    raw = tmp_path / "raw" / "runtime_images.json"
    for evidence in ([], {"run_id": manifest["run_id"], "valid": True,
                          "required": [], "matches": []}):
        raw.write_text(json.dumps(evidence))
        manifest["runtime_images"] = {
            "content_sha256": core.hashlib.sha256(raw.read_bytes()).hexdigest(),
            "evidence": evidence,
        }
        (tmp_path / "manifest.json").write_text(json.dumps(manifest))
        assert core._valid_runtime_images(manifest["runtime_images"], manifest, tmp_path) is False


def test_required_core_metrics_fails_closed(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["core_metrics"] = {"required": True, "status": "stale"}
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    result = validate_dir(tmp_path)
    assert result["valid_for_comparison"] is False
    assert "CORE_METRICS_REQUIRED" in result["failure_codes"]


def test_required_core_metrics_missing_archive_fails_closed(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["core_metrics"] = {"required": True, "status": "supported"}
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    assert validate_dir(tmp_path)["valid_for_comparison"] is False


def _add_required_core(path):
    record = {"schema": "ibmw.core_metrics.v1", "snapshot_seq": 1, "kind": "latency",
              "cumulative": {}, "complete": True}
    artifact = path/"raw/core_metrics.jsonl"
    artifact.write_text(json.dumps(record) + "\n")
    manifest = json.loads((path/"manifest.json").read_text())
    digest = core.hashlib.sha256(artifact.read_bytes()).hexdigest()
    metadata = {"exists": True, "path": "/source/metrics.jsonl", "mtime_ns": 1, "size": artifact.stat().st_size, "inode": 1}
    manifest["adapter"] = "ibmw"
    manifest["mode"] = "managed"
    manifest["pid"] = 123
    manifest["pid_start_time_ticks"] = 456
    manifest["root"] = "/source"
    manifest["build"] = {}
    manifest["core_metrics"] = {"required": True, "status": "supported", "record_count": 1,
                                "kinds": ["latency"], "content_sha256": digest,
                                "provenance": {"run_id": manifest["run_id"], "adapter": "ibmw",
                                    "mode": "managed", "sut_pid": 123, "sut_start_time_ticks": 456,
                                    "build": {}, "root": "/source", "source_path": "/source/metrics.jsonl",
                                    "source_sha256": digest,
                                    "archive_sha256": digest, "baseline_metadata": metadata,
                                    "final_metadata": metadata, "graceful_finalization": True,
                                    "final_snapshot": True}}
    (path/"manifest.json").write_text(json.dumps(manifest))
    return artifact


def test_required_core_metrics_revalidates_archive_and_manifest(tmp_path):
    for mutation in ("delete", "truncate", "count", "kinds", "hash", "missing_hash"):
        case = tmp_path/mutation; case.mkdir(); _write_valid_artifacts(case)
        artifact = _add_required_core(case)
        if mutation == "delete": artifact.unlink()
        elif mutation == "truncate": artifact.write_bytes(artifact.read_bytes()[:-1])
        else:
            manifest = json.loads((case/"manifest.json").read_text())
            key = {"count": "record_count", "hash": "content_sha256"}.get(mutation, mutation)
            if mutation == "missing_hash":
                manifest["core_metrics"].pop("content_sha256")
            else:
                manifest["core_metrics"][key] = (
                    2 if mutation == "count" else ["wrong"] if mutation == "kinds" else "0" * 64)
            (case/"manifest.json").write_text(json.dumps(manifest))
        assert validate_dir(case)["valid_for_comparison"] is False, mutation


def test_archive_core_metrics_output_round_trips_through_validate_dir(tmp_path):
    _write_valid_artifacts(tmp_path)
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    source = source_dir / "metrics.jsonl"
    baseline = adapters.ibmw.core_metrics_snapshot({"directory": source_dir, "required": True})
    source.write_text(json.dumps({"schema": "ibmw.core_metrics.v1", "snapshot_seq": 1,
                                  "kind": "latency", "cumulative": {}, "complete": True}) + "\n" +
                      json.dumps({"schema": "ibmw.core_metrics.v1", "snapshot_seq": 2,
                                  "kind": "future_kind", "cumulative": {}, "complete": True}) + "\n")
    archived = adapters.ibmw.archive_core_metrics(
        {"directory": source_dir, "required": True}, baseline,
        tmp_path / "raw" / "core_metrics.jsonl", managed=True, run_id="test-run",
        adapter="ibmw", build={}, root="/source", mode="managed", sut_pid=123,
        sut_start_time_ticks=456)
    assert archived["status"] == "supported"

    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest.update(adapter="ibmw", mode="managed", pid=123, pid_start_time_ticks=456,
                    root="/source", build={},
                    core_metrics={"required": True, **{
                        key: archived[key] for key in
                        ("status", "record_count", "kinds", "content_sha256", "provenance")
                    }, "source_path": archived["source_path"]})
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))
    validation = validate_dir(tmp_path)
    assert validation["core_metrics"]["valid"] is True
    assert validation["valid_for_metric"] is True
    assert validation["valid_for_comparison"] is True
    assert archived["provenance"]["source_sha256"] == archived["provenance"]["archive_sha256"]
    assert archived["provenance"]["graceful_finalization"] is True
    assert archived["provenance"]["final_snapshot"] is True


def test_required_core_metrics_provenance_hash_and_finalization_closure(tmp_path):
    for name, mutate in (
        ("source_hash", lambda p: p.update(source_sha256="0" * 64)),
        ("archive_hash", lambda p: p.update(archive_sha256="0" * 64)),
        ("graceful", lambda p: p.update(graceful_finalization=False)),
        ("final_snapshot", lambda p: p.update(final_snapshot=False)),
        ("missing_graceful", lambda p: p.pop("graceful_finalization")),
        ("missing_final_snapshot", lambda p: p.pop("final_snapshot")),
    ):
        case = tmp_path / name
        case.mkdir()
        _write_valid_artifacts(case)
        _add_required_core(case)
        manifest = json.loads((case / "manifest.json").read_text())
        mutate(manifest["core_metrics"]["provenance"])
        (case / "manifest.json").write_text(json.dumps(manifest))
        result = validate_dir(case)
        assert result["valid_for_collection"] is True
        assert result["valid_for_metric"] is False
        assert result["valid_for_comparison"] is False


def test_complete_core_metrics_artifact_does_not_make_unsupported_metric_available(tmp_path):
    _write_valid_artifacts(tmp_path)
    _add_required_core(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest["metric_availability"] = "unsupported"
    manifest["metrics_valid"] = True
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))

    result = validate_dir(tmp_path)
    assert result["core_metrics"]["valid"] is True
    assert result["valid_for_collection"] is True
    assert result["valid_for_metric"] is False
    assert result["valid_for_comparison"] is False
    assert next(rule for rule in result["rules"] if rule["name"] == "metric_available")["passed"] is False
    assert "METRIC_AVAILABLE" in result["failure_codes"]


def test_optional_core_metrics_never_blocks_validation(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["core_metrics"] = {"required": False, "status": "invalid", "record_count": 99, "kinds": []}
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    (tmp_path/"raw/core_metrics.jsonl").write_text("truncated")
    assert validate_dir(tmp_path)["valid_for_comparison"] is True


def test_optional_latency_diagnostics_do_not_block_metadata_or_claim_measurement(tmp_path):
    for status in ("partial", "legacy"):
        case = tmp_path/status; case.mkdir(); _write_valid_artifacts(case)
        manifest = json.loads((case/"manifest.json").read_text())
        manifest["measurement_kind"] = "metadata"
        manifest["latency_artifact"] = {"required": False, "status": status}
        # An archived legacy/partial artifact is diagnostic evidence, not a
        # metric/comparison conclusion.  Missing optional metadata remains
        # compatible and is covered by the ordinary fixture above.
        (case/"raw/latency.jsonl").write_text("legacy artifact\n")
        (case/"manifest.json").write_text(json.dumps(manifest))
        result = validate_dir(case)
        assert result["valid_for_collection"] is True, status
        assert result["valid_for_metric"] is False, status
        assert result["valid_for_comparison"] is False, status

        manifest["measurement_kind"] = "producer_latency"
        (case/"manifest.json").write_text(json.dumps(manifest))
        assert validate_dir(case)["valid_for_comparison"] is False, status


def test_optional_archived_unknown_ros2_clock_domain_is_not_metric_eligible(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest.update(adapter="ros2", measurement_kind="metadata",
                    latency_artifact={"required": False, "status": "invalid",
                                      "clock_domain": "wall_clock"})
    latency = [
        {"schema": "mw_perf.latency.v1", "seq": 0, "send_ns": 10,
         "receive_ns": 20, "latency_ns": 10, "topic": "/chatter",
         "type": "std_msgs/msg/String", "payload_bytes": 4, "valid": True},
        {"schema": "mw_perf.latency.v1", "kind": "completion", "run_id": "test-run",
         "producer_pid": 1, "producer_start_time_ticks": 2, "record_count": 1,
         "topic": "/chatter", "type": "std_msgs/msg/String", "complete": True,
         "clock_domain": "wall_clock"},
    ]
    (tmp_path / "raw/latency.jsonl").write_text("\n".join(json.dumps(item) for item in latency) + "\n")
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))

    result = validate_dir(tmp_path)
    assert result["valid_for_collection"] is True
    assert result["valid_for_metric"] is False
    assert result["valid_for_comparison"] is False


def test_completion_only_latency_cannot_support_manifest_metric(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest.update(adapter="ros2", measurement_kind="producer_latency",
                    metric_availability="supported",
                    latency_artifact={"required": False, "status": "supported",
                                      "record_count": 0, "topic": "/chatter",
                                      "type": "std_msgs/msg/String"})
    completion_only = {
        "schema": "mw_perf.latency.v1", "kind": "completion", "run_id": "test-run",
        "producer_pid": 1, "producer_start_time_ticks": 2, "record_count": 0,
        "topic": "/chatter", "type": "std_msgs/msg/String", "complete": True,
        "clock_domain": "producer_monotonic",
    }
    (tmp_path / "raw/latency.jsonl").write_text(json.dumps(completion_only) + "\n")
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))

    result = validate_dir(tmp_path)
    assert result["latency_artifact"]["valid"] is False
    assert result["valid_for_metric"] is False
    assert result["valid_for_comparison"] is False


def test_sigkill_makes_required_archive_invalid(tmp_path):
    from mw_perf_collect.adapters.ibmw import archive_core_metrics, core_metrics_snapshot
    directory = tmp_path/"source"; directory.mkdir()
    source = directory/"metrics.jsonl"
    baseline = core_metrics_snapshot({"directory": directory, "required": True})
    source.write_text(json.dumps({"schema": "ibmw.core_metrics.v1", "snapshot_seq": 1,
                                  "kind": "latency", "cumulative": {}, "complete": True}) + "\n")
    result = archive_core_metrics({"directory": directory, "required": True}, baseline,
                                  tmp_path/"archive.jsonl", managed=True, graceful_finalization=False)
    assert result["status"] == "invalid"
    assert result["provenance"]["graceful_finalization"] is False


def test_required_core_metrics_rejects_invalid_provenance(tmp_path):
    _write_valid_artifacts(tmp_path)
    _add_required_core(tmp_path)
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["core_metrics"]["provenance"]["run_id"] = "wrong-run"
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    assert validate_dir(tmp_path)["valid_for_comparison"] is False


def test_required_core_metrics_rejects_identity_metadata_and_path_tampering(tmp_path):
    mutations = (
        ("sut_pid", 999), ("sut_start_time_ticks", 999), ("source_path", "/wrong/metrics.jsonl"),
        ("baseline_metadata", {"exists": "yes", "path": "/source/metrics.jsonl",
                               "mtime_ns": 1, "size": 1, "inode": 1}),
    )
    for name, value in mutations:
        case = tmp_path/name; case.mkdir(); _write_valid_artifacts(case); _add_required_core(case)
        manifest = json.loads((case/"manifest.json").read_text())
        manifest["core_metrics"]["provenance"][name] = value
        (case/"manifest.json").write_text(json.dumps(manifest))
        assert validate_dir(case)["valid_for_comparison"] is False, name


def test_report_shows_core_metrics_provenance(tmp_path):
    _write_valid_artifacts(tmp_path)
    _add_required_core(tmp_path)
    core.report(tmp_path)
    text = (tmp_path/"report.md").read_text()
    assert "Core Metrics source SHA-256" in text
    assert "Core Metrics graceful finalization: `True`" in text


def test_candidate_only_marker_invalidates_comparison_not_collection(tmp_path):
    _write_valid_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    manifest["candidate_only"] = True
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))

    validation = validate_dir(tmp_path)
    assert validation["valid_for_collection"] is True
    assert validation["valid_for_comparison"] is False
    assert "CANDIDATE_ONLY" in validation["failure_codes"]
    core.report(tmp_path)
    assert "candidate only: `True`" in (tmp_path / "report.md").read_text()


def test_required_provenance_rejects_binary_source_and_scenario_tampering(tmp_path, monkeypatch):
    _write_valid_artifacts(tmp_path)
    binary = tmp_path/"sut"; binary.write_bytes(b"binary")
    cache_dir = tmp_path/"build"; cache_dir.mkdir(); cache = cache_dir/"CMakeCache.txt"; cache.write_text("X:BOOL=ON\n")
    contract = {"deployment": {"hosts": 1}, "workload": {"count": 3}}
    source = {"root": str(tmp_path), "commit": "a" * 40, "branch": "main", "dirty": False, "diff_sha256": "b" * 64}
    from mw_perf_collect.adapters.ibmw import binary_identity
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["adapter"] = "ibmw"
    manifest["scenario_contract"] = contract
    manifest["scenario_contract_hash"] = core._hash_contract({"scenario_contract": contract})
    identity = binary_identity(binary)
    manifest["provenance"] = {"required": True, "runtime_images_required": True,
        "binaries": [identity], "source": source,
        "build": {"configured": True, "build_dir": str(cache_dir), "cache_sha256": core.hashlib.sha256(cache.read_bytes()).hexdigest()}}
    observation = {"run_id": manifest["run_id"], "pid": manifest["pid"],
                   "start_time_ticks": manifest["pid_start_time_ticks"], "source": "exe",
                   "identity_source": "proc_exe", **core._file_identity(binary)}
    declaration = [{"role": "sut", "name": "sut", "source": "exe",
                    "expected_path": str(binary)}]
    runtime = core._runtime_image_result(declaration, [observation], manifest["run_id"], [identity])
    core.write_json(tmp_path/"raw/runtime_images.json", runtime)
    manifest["runtime_images"] = {
        "content_sha256": core.hashlib.sha256((tmp_path/"raw/runtime_images.json").read_bytes()).hexdigest(),
        "evidence": runtime,
    }
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    monkeypatch.setattr("mw_perf_collect.adapters.ibmw.source_identity", lambda _: dict(source))
    validation = validate_dir(tmp_path)
    assert validation["valid_for_comparison"] is True, validation
    original = json.loads((tmp_path/"manifest.json").read_text())
    for mutation in ("binary", "source", "scenario"):
        changed = json.loads(json.dumps(original))
        if mutation == "binary": changed["provenance"]["binaries"][0]["sha256"] = "0" * 64
        elif mutation == "source": changed["provenance"]["source"]["commit"] = "0" * 40
        else: changed["scenario_contract"]["workload"]["count"] = 4
        (tmp_path/"manifest.json").write_text(json.dumps(changed))
        assert validate_dir(tmp_path)["valid_for_comparison"] is False, mutation


def test_ibmw_provenance_revalidates_binary_device_and_inode(tmp_path, monkeypatch):
    from mw_perf_collect.adapters.ibmw import binary_identity

    binary = tmp_path/"sut"; binary.write_bytes(b"binary")
    identity = binary_identity(binary)
    source = {"root": str(tmp_path), "commit": "a" * 40}
    cache_dir = tmp_path/"build"; cache_dir.mkdir()
    cache = cache_dir/"CMakeCache.txt"; cache.write_text("X:BOOL=ON\n")
    provenance = {
        "required": True,
        "binaries": [identity],
        "source": source,
        "build": {"configured": True, "build_dir": str(cache_dir),
                  "cache_sha256": core.hashlib.sha256(cache.read_bytes()).hexdigest()},
    }
    monkeypatch.setattr("mw_perf_collect.adapters.ibmw.source_identity", lambda _: dict(source))

    assert core._valid_ibmw_provenance(provenance) is True
    replacement = tmp_path/"replacement"; replacement.write_bytes(binary.read_bytes())
    os.utime(replacement, ns=(identity["mtime_ns"], identity["mtime_ns"]))
    replacement.replace(binary)
    current = binary_identity(binary)
    assert {key: current[key] for key in ("declared_path", "resolved_path", "size", "mtime_ns", "sha256")} == {
        key: identity[key] for key in ("declared_path", "resolved_path", "size", "mtime_ns", "sha256")}
    assert current["inode"] != identity["inode"]
    assert core._valid_ibmw_provenance(provenance) is False

    provenance["binaries"] = [current]
    assert core._valid_ibmw_provenance(provenance) is True


def test_ibmw_provenance_binary_missing_device_or_inode_fails_closed(tmp_path, monkeypatch):
    from mw_perf_collect.adapters.ibmw import binary_identity

    binary = tmp_path/"sut"; binary.write_bytes(b"binary")
    source = {"root": str(tmp_path)}
    cache_dir = tmp_path/"build"; cache_dir.mkdir()
    cache = cache_dir/"CMakeCache.txt"; cache.write_text("X:BOOL=ON\n")
    provenance = {
        "required": True,
        "binaries": [binary_identity(binary)],
        "source": source,
        "build": {"configured": True, "build_dir": str(cache_dir),
                  "cache_sha256": core.hashlib.sha256(cache.read_bytes()).hexdigest()},
    }
    monkeypatch.setattr("mw_perf_collect.adapters.ibmw.source_identity", lambda _: dict(source))

    for field in ("device", "inode"):
        changed = json.loads(json.dumps(provenance))
        changed["binaries"][0].pop(field)
        assert core._valid_ibmw_provenance(changed) is False


def test_stop_managed_gives_sigkill_an_independent_wait_window(monkeypatch):
    class Proc:
        pid = 42
        returncode = None
        wait_timeout = None
        def poll(self): return self.returncode
        def wait(self, timeout): self.wait_timeout = timeout; return 0

    proc = Proc()
    clock = iter((0.0, 0.0, 0.6, 1.1, 1.1, 1.6, 2.2))
    sent = []
    monkeypatch.setattr(core.time, "monotonic", lambda: next(clock))
    monkeypatch.setattr(core.time, "sleep", lambda _: None)
    monkeypatch.setattr(core, "_same_process", lambda *_: True)
    monkeypatch.setattr(core.os, "killpg", lambda *_: None)
    monkeypatch.setattr(core.os, "kill", lambda pid, sig: sent.append((pid, sig)))

    error = core._stop_managed(cast(Any, proc), 1.0, {42: 7})

    assert (42, signal.SIGKILL) in sent
    assert proc.wait_timeout == 1.0
    assert error == "processes remain: 42"


def test_runtime_images_reject_unobserved_and_replaced_files(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"original")
    identity = core._file_identity(image)
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **identity}
    result = core._runtime_image_result([{"role": "sut", "name": "sut", "source": "exe",
        "expected_path": str(image)}], [observed], "run")
    assert result["valid"] is True
    assert core._runtime_image_result([{"role": "other", "name": "missing", "source": "exe",
        "expected_path": str(tmp_path/"missing")}], [observed], "run")["valid"] is False
    value = _write_runtime_evidence(tmp_path, result)
    assert core._valid_runtime_images(value, _runtime_manifest(binary=identity), tmp_path)
    image.write_bytes(b"tampered")
    assert core._valid_runtime_images(value, _runtime_manifest(binary=identity), tmp_path)


def test_runtime_images_reject_pid_start_and_run_id_tampering(tmp_path):
    image = tmp_path/"plugin.so"; image.write_bytes(b"plugin")
    observation = {"run_id": "run", "pid": 11, "start_time_ticks": 22, "source": "maps",
                   "identity_source": "proc_map_file", **core._file_identity(image)}
    observation["mapping"] = {"start_address": "1000", "end_address": "2000",
                              "permissions": "r-xp", "device": "00:00",
                              "inode": observation["inode"], "deleted": False}
    for field, value in (("run_id", "other"), ("pid", "11"), ("start_time_ticks", "22")):
        changed = dict(observation); changed[field] = value
        result = core._runtime_image_result([{"role":"plugin", "name":"plugin", "source":"maps",
            "expected_path":str(image)}], [changed], "run")
        raw = tmp_path/field; raw.mkdir(); (raw/"raw").mkdir(); core.write_json(raw/"raw/runtime_images.json", result)
        data = (raw/"raw/runtime_images.json").read_bytes()
        assert not core._valid_runtime_images({"content_sha256":core.hashlib.sha256(data).hexdigest(),
            "evidence":result}, _runtime_manifest(11, 22), raw)


def _write_runtime_evidence(path, evidence, sampled=None):
    (path/"raw").mkdir(parents=True)
    identities = sampled if sampled is not None else [
        {"pid": item["pid"], "start_time_ticks": item["start_time_ticks"]}
        for item in evidence.get("observations", []) if isinstance(item, dict)
    ]
    (path/"raw/process.jsonl").write_text(json.dumps({"timestamp": 1.0, "processes": identities}) + "\n")
    core.write_json(path/"raw/runtime_images.json", evidence)
    data = (path/"raw/runtime_images.json").read_bytes()
    return {"content_sha256": core.hashlib.sha256(data).hexdigest(), "evidence": evidence}


def _runtime_manifest(pid=10, start_time_ticks=20, binary=None):
    binaries = [] if binary is None else [dict(binary)]
    return {"run_id": "run", "pid": pid, "pid_start_time_ticks": start_time_ticks,
            "provenance": {"binaries": binaries}}


def test_runtime_image_declaration_non_object_fails_closed(tmp_path):
    result = core._runtime_image_result(["not-an-object"], [], "run")
    assert result["valid"] is False
    value = _write_runtime_evidence(tmp_path, result)
    assert core._valid_runtime_images(value, _runtime_manifest(), tmp_path) is False


def test_runtime_images_reject_observed_source_tampering(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **core._file_identity(image)}
    result = core._runtime_image_result([{"role": "sut", "name": "sut", "source": "exe",
        "expected_path": str(image)}], [observed], "run")
    result["observations"][0]["source"] = "maps"
    value = _write_runtime_evidence(tmp_path/"source", result)
    assert core._valid_runtime_images(value, _runtime_manifest(), tmp_path/"source") is False


def test_runtime_images_reject_resolved_path_not_matching_normalized_expected_path(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    other = tmp_path/"other"; other.write_bytes(b"binary")
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **core._file_identity(image)}
    result = core._runtime_image_result([{"role": "sut", "name": "sut", "source": "exe",
        "expected_path": str(image)}], [observed], "run")
    result["required"][0]["expected_path"] = str(other)
    result["matches"][0]["expected_path"] = str(other)
    value = _write_runtime_evidence(tmp_path/"path", result)
    assert core._valid_runtime_images(value, _runtime_manifest(), tmp_path/"path") is False


def test_runtime_images_reject_declared_sha256_mismatch(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **core._file_identity(image)}
    result = core._runtime_image_result([{"role": "sut", "name": "sut", "source": "exe",
        "expected_path": str(image)}], [observed], "run")
    result["required"][0]["sha256"] = "0" * 64
    result["matches"][0]["expected_sha256"] = "0" * 64
    value = _write_runtime_evidence(tmp_path/"sha", result)
    assert core._valid_runtime_images(value, _runtime_manifest(), tmp_path/"sha") is False


def test_deleted_map_uses_mapped_file_not_replacement_path(tmp_path, monkeypatch):
    image = tmp_path/"plugin.so"; image.write_bytes(b"mapped-original")
    fd = os.open(image, os.O_RDONLY); mapped = os.fstat(fd)
    image.unlink(); image.write_bytes(b"replacement")
    real_open_identity = core._open_identity

    def open_identity(path, resolved_path, expected=None):
        if "/map_files/" in path:
            return core._fd_identity(fd, resolved_path)
        return real_open_identity(path, resolved_path, expected)

    monkeypatch.setattr(core, "_open_identity", open_identity)
    device = f"{os.major(mapped.st_dev):02x}:{os.minor(mapped.st_dev):02x}"
    try:
        observed = core._maps_identity(77, ["1000-2000", "r-xp", "0", device,
                                                   str(mapped.st_ino), f"{image} (deleted)"])
    finally:
        os.close(fd)
    assert observed is not None
    assert observed["sha256"] == core.hashlib.sha256(b"mapped-original").hexdigest()
    assert observed["sha256"] != core.hashlib.sha256(image.read_bytes()).hexdigest()
    assert observed["device"] == mapped.st_dev and observed["inode"] == mapped.st_ino
    assert observed["mapping"]["deleted"] is True
    assert observed["mapping"]["permissions"] == "r-xp"
    observation = {"run_id": "run", "pid": 77, "start_time_ticks": 88, "source": "maps", **observed}
    expected_sha = core.hashlib.sha256(b"mapped-original").hexdigest()
    result = core._runtime_image_result(
        [{"role": "plugin", "name": "plugin", "source": "maps",
          "expected_path": str(image), "sha256": expected_sha}], [observation], "run")
    value = _write_runtime_evidence(tmp_path/"evidence", result)
    assert core._valid_runtime_images(
        value, _runtime_manifest(77, 88, observed), tmp_path/"evidence")


def test_map_file_failure_falls_back_to_matching_proc_root_identity(tmp_path, monkeypatch):
    image = tmp_path/"plugin.so"; image.write_bytes(b"mapped-image")
    info = image.stat(); calls = []

    def open_identity(path, resolved, expected):
        calls.append(path)
        if "/map_files/" in path:
            raise PermissionError()
        return {"resolved_path": resolved, "device": info.st_dev, "inode": info.st_ino,
                "size": info.st_size, "sha256": core.hashlib.sha256(image.read_bytes()).hexdigest()}

    monkeypatch.setattr(core, "_open_identity", open_identity)
    device = f"{os.major(info.st_dev):02x}:{os.minor(info.st_dev):02x}"
    observed = core._maps_identity(77, ["1000-2000", "r-xp", "0", device,
                                         str(info.st_ino), str(image)])
    assert observed is not None
    assert observed["identity_source"] == "proc_root_path"
    assert calls == ["/proc/77/map_files/1000-2000", f"/proc/77/root{image}"]


def test_proc_root_fallback_rejects_device_or_inode_mismatch(tmp_path, monkeypatch):
    image = tmp_path/"plugin.so"; image.write_bytes(b"replacement")
    info = image.stat()
    device = f"{os.major(info.st_dev):02x}:{os.minor(info.st_dev):02x}"

    for observed_device, observed_inode in ((info.st_dev + 1, info.st_ino),
                                             (info.st_dev, info.st_ino + 1)):
        def open_identity(path, resolved, expected):
            if "/map_files/" in path:
                raise PermissionError()
            return {"resolved_path": resolved, "device": observed_device, "inode": observed_inode,
                    "size": info.st_size, "sha256": "0" * 64}

        monkeypatch.setattr(core, "_open_identity", open_identity)
        assert core._maps_identity(77, ["1000-2000", "r-xp", "0", device,
                                        str(info.st_ino), str(image)]) is None


def test_deleted_map_file_failure_does_not_call_proc_root_fallback(tmp_path, monkeypatch):
    image = tmp_path/"plugin.so"; image.write_bytes(b"replacement")
    info = image.stat(); calls = []

    def open_identity(path, resolved, expected):
        calls.append(path)
        raise PermissionError()

    monkeypatch.setattr(core, "_open_identity", open_identity)
    device = f"{os.major(info.st_dev):02x}:{os.minor(info.st_dev):02x}"
    assert core._maps_identity(77, ["1000-2000", "r-xp", "0", device,
                                    str(info.st_ino), f"{image} (deleted)"]) is None
    assert calls == ["/proc/77/map_files/1000-2000"]


def test_maps_identity_rejects_opened_device_mismatch(monkeypatch):
    mapped_device = "08:01"
    mapped_inode = 123
    expected_device = os.makedev(0x08, 0x01)
    monkeypatch.setattr(core, "_open_identity", lambda *_: {
        "resolved_path": "/source/plugin.so", "device": expected_device + 1,
        "inode": mapped_inode, "size": 10, "sha256": "0" * 64,
    })

    assert core._maps_identity(77, ["1000-2000", "r-xp", "0", mapped_device,
                                    str(mapped_inode), "/source/plugin.so"]) is None


def test_maps_identity_rejects_opened_inode_mismatch(monkeypatch):
    mapped_device = "08:01"
    mapped_inode = 123
    expected_device = os.makedev(0x08, 0x01)
    monkeypatch.setattr(core, "_open_identity", lambda *_: {
        "resolved_path": "/source/plugin.so", "device": expected_device,
        "inode": mapped_inode + 1, "size": 10, "sha256": "0" * 64,
    })

    assert core._maps_identity(77, ["1000-2000", "r-xp", "0", mapped_device,
                                    str(mapped_inode), "/source/plugin.so"]) is None


def test_runtime_images_require_top_level_observation_ids(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **core._file_identity(image)}
    declaration = [{"role": "sut", "name": "sut", "source": "exe", "expected_path": str(image)}]
    original = core._runtime_image_result(declaration, [observed], "run")
    cases = []
    spliced = json.loads(json.dumps(original)); spliced["matches"][0]["observed"] = [observed]; cases.append(spliced)
    empty = json.loads(json.dumps(original)); empty["observations"] = []; cases.append(empty)
    missing = json.loads(json.dumps(original)); missing["matches"][0]["observed"] = ["f" * 64]; cases.append(missing)
    for index, evidence in enumerate(cases):
        case = tmp_path/str(index)
        value = _write_runtime_evidence(case, evidence)
        assert core._valid_runtime_images(value, _runtime_manifest(), case) is False


def test_runtime_images_reject_fictional_pid_not_in_samples(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    observed = {"run_id": "run", "pid": 999, "start_time_ticks": 888, "source": "exe",
                "identity_source": "proc_exe", **core._file_identity(image)}
    result = core._runtime_image_result(
        [{"role": "sut", "name": "sut", "source": "exe", "expected_path": str(image)}],
        [observed], "run")
    value = _write_runtime_evidence(tmp_path, result, sampled=[{"pid": 10, "start_time_ticks": 20}])
    assert core._valid_runtime_images(
        value, _runtime_manifest(10, 20, core._file_identity(image)), tmp_path) is False


def test_runtime_images_unmatched_diagnostic_pid_does_not_block_validation(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    identity = core._file_identity(image)
    matched = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
               "identity_source": "proc_exe", **identity}
    diagnostic = {"run_id": "run", "pid": 999, "start_time_ticks": 888, "source": "exe",
                  "identity_source": "proc_exe", **identity, "resolved_path": "/diagnostic"}
    declaration = [{"role": "sut", "name": "sut", "source": "exe",
                    "expected_path": str(image)}]
    result = core._runtime_image_result(declaration, [matched, diagnostic], "run", [identity])
    assert result["matches"][0]["expected_sha256"] == identity["sha256"]
    value = _write_runtime_evidence(
        tmp_path, result, sampled=[{"pid": 10, "start_time_ticks": 20}])
    assert core._valid_runtime_images(value, _runtime_manifest(binary=identity), tmp_path)


def test_runtime_images_reject_ambiguous_binary_identity_when_sha_omitted(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    identity = core._file_identity(image)
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **identity}
    declaration = [{"role": "sut", "name": "sut", "source": "exe",
                    "expected_path": str(image)}]
    result = core._runtime_image_result(declaration, [observed], "run")
    value = _write_runtime_evidence(tmp_path, result)
    conflicting = {**identity, "sha256": "0" * 64}
    manifest = _runtime_manifest(binary=identity)
    manifest["provenance"]["binaries"].append(conflicting)
    assert core._valid_runtime_images(value, manifest, tmp_path) is False


def test_runtime_images_require_manifest_binary_device_and_inode(tmp_path):
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    identity = core._file_identity(image)
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "exe",
                "identity_source": "proc_exe", **identity}
    declaration = [{"role": "sut", "name": "sut", "source": "exe",
                    "expected_path": str(image)}]
    result = core._runtime_image_result(declaration, [observed], "run", [identity])
    value = _write_runtime_evidence(tmp_path, result)
    assert core._valid_runtime_images(value, _runtime_manifest(binary=identity), tmp_path)

    for field in ("device", "inode"):
        incomplete = dict(identity); incomplete.pop(field)
        assert core._valid_runtime_images(
            value, _runtime_manifest(binary=incomplete), tmp_path) is False


def test_read_only_maps_observation_cannot_satisfy_runtime_image(tmp_path):
    image = tmp_path/"data.bin"; image.write_bytes(b"read-only mapping")
    identity = core._file_identity(image)
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "maps",
                "identity_source": "proc_map_file", **identity,
                "mapping": {"start_address": "1000", "end_address": "2000",
                            "permissions": "r--p",
                            "device": f"{os.major(identity['device']):02x}:{os.minor(identity['device']):02x}",
                            "inode": identity["inode"], "deleted": False}}
    declaration = [{"role": "data", "name": "data", "source": "maps",
                    "expected_path": str(image)}]
    result = core._runtime_image_result(declaration, [observed], "run", [identity])
    assert result["valid"] is False
    value = _write_runtime_evidence(tmp_path, result)
    assert core._valid_runtime_images(value, _runtime_manifest(binary=identity), tmp_path) is False


def test_runtime_image_validator_rejects_non_executable_or_malformed_map_permissions(tmp_path):
    image = tmp_path/"plugin.so"; image.write_bytes(b"plugin")
    identity = core._file_identity(image)
    observed = {"run_id": "run", "pid": 10, "start_time_ticks": 20, "source": "maps",
                "identity_source": "proc_map_file", **identity,
                "mapping": {"start_address": "1000", "end_address": "2000",
                            "permissions": "r-xp",
                            "device": f"{os.major(identity['device']):02x}:{os.minor(identity['device']):02x}",
                            "inode": identity["inode"], "deleted": False}}
    declaration = [{"role": "plugin", "name": "plugin", "source": "maps",
                    "expected_path": str(image)}]
    original = core._runtime_image_result(declaration, [observed], "run", [identity])
    assert original["valid"] is True
    for index, permissions in enumerate(("r--p", "rwx")):
        evidence = json.loads(json.dumps(original))
        evidence["observations"][0]["mapping"]["permissions"] = permissions
        observation_id = core._observation_id(evidence["observations"][0])
        evidence["observations"][0]["observation_id"] = observation_id
        evidence["matches"][0]["observed"] = [observation_id]
        case = tmp_path/str(index)
        value = _write_runtime_evidence(case, evidence)
        assert core._valid_runtime_images(
            value, _runtime_manifest(binary=identity), case) is False


def test_required_ibmw_runtime_gate_does_not_trust_manifest_flag(tmp_path, monkeypatch):
    _write_valid_artifacts(tmp_path)
    image = tmp_path/"sut"; image.write_bytes(b"binary")
    identity = core._file_identity(image)
    manifest = json.loads((tmp_path/"manifest.json").read_text())
    manifest["adapter"] = "ibmw"
    manifest["scenario_contract"] = {"workload": {"count": 1}}
    manifest["scenario_contract_hash"] = core._hash_contract(
        {"scenario_contract": manifest["scenario_contract"]})
    manifest["provenance"] = {"required": True, "runtime_images_required": True,
                              "binaries": [identity]}
    observed = {"run_id": manifest["run_id"], "pid": manifest["pid"],
                "start_time_ticks": manifest["pid_start_time_ticks"], "source": "exe",
                "identity_source": "proc_exe", **identity}
    declaration = [{"role": "sut", "name": "sut", "source": "exe",
                    "expected_path": str(image)}]
    evidence = core._runtime_image_result(declaration, [observed], manifest["run_id"], [identity])
    core.write_json(tmp_path/"raw/runtime_images.json", evidence)
    manifest["runtime_images"] = {
        "content_sha256": core.hashlib.sha256((tmp_path/"raw/runtime_images.json").read_bytes()).hexdigest(),
        "evidence": evidence,
    }
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    monkeypatch.setattr(core, "_valid_ibmw_provenance", lambda _: True)
    assert validate_dir(tmp_path)["valid_for_comparison"] is True

    manifest["provenance"].pop("runtime_images_required")
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    assert validate_dir(tmp_path)["valid_for_comparison"] is True

    manifest.pop("runtime_images")
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    result = validate_dir(tmp_path)
    assert result["valid_for_comparison"] is False
    assert "RUNTIME_IMAGES" in result["failure_codes"]

    manifest["runtime_images"] = {"content_sha256": core.hashlib.sha256(
        (tmp_path/"raw/runtime_images.json").read_bytes()).hexdigest(), "evidence": evidence}
    (tmp_path/"manifest.json").write_text(json.dumps(manifest))
    (tmp_path/"raw/runtime_images.json").unlink()
    result = validate_dir(tmp_path)
    assert result["valid_for_comparison"] is False
    assert "RUNTIME_IMAGES" in result["failure_codes"]


def test_required_ibmw_run_preflight_rejects_missing_runtime_declarations(tmp_path, capsys):
    config = tmp_path/"config.json"
    config.write_text(json.dumps({
        "sut": {"mode": "managed", "command": ["/bin/true"]},
        "scenario_contract": {"workload": {"count": 1}},
        "adapters": {"ibmw": {"provenance": {"required": True}}},
    }))
    args = core.argparse.Namespace(config=str(config), adapter="ibmw", command=[],
                                   override_command=False, output=str(tmp_path/"runs"))
    assert core.run(args) == core.EXIT_FAILED
    assert "required IBMW provenance needs non-empty runtime_images declarations" in capsys.readouterr().err
    assert not (tmp_path/"runs").exists()


def _ros2_preflight_value(*, identity_valid=True, libraries_verified=True,
                          tracing_available=False):
    return {
        "schema": "mw_perf.ros2.preflight.v1", "pid": 123,
        "ros2_command": {"path": "/trusted/ros2", "trusted": True},
        "environment": {"prefixes": []},
        "trusted_runtime": {"status": "unconfigured", "prefix": None,
            "executable": None, "interpreter": None, "shebang_interpreter": None,
            "python_version": None, "python_paths": [], "library_paths": [],
            "identity": None, "reason": "ros2_prefix is not configured"},
        "ros2_pkg_prefix": [],
        "process_identity": {"start_ticks_before": 1, "start_ticks_after": 1,
                             "valid": identity_valid, "reason": None},
        "process": {"maps": [], "dynamic_library_resolution": {
                        "name": "dynamic_library_resolution",
                        "metric_availability": "not_available", "availability": "not_available"},
                    "resolved_libraries": []},
        "ros2_tracing": {"commands": [], "command_available": True,
            "session_command_usable": tracing_available, "tracepoints_enumerated": True,
            "required_tracepoints_present": tracing_available,
            "required_tracepoints": ["ros2:rcl_publish"],
            "missing_required_tracepoints": [], "available": tracing_available,
            "tracepoints": ["ros2:rcl_publish"]},
        "middleware_libraries": [],
        "required_runtime_libraries": {"configured": True,
            "all_capability_verified": libraries_verified,
            "matches": [{"id": "fastdds", "matched": libraries_verified}]},
        "claims": {"exact": False, "abi_inferred_from_version_name": False},
    }


def test_ros2_preflight_gate_only_blocks_required_runs():
    result = {"observations": [{"name": "ros2_preflight", "value":
                                _ros2_preflight_value(identity_valid=False,
                                                      libraries_verified=False)}]}
    assert core._ros2_preflight_gate(result, {}, False)["ok"] is True
    gate = core._ros2_preflight_gate(result, {}, True)
    assert gate["ok"] is False
    assert {"pid_identity_unstable", "required_runtime_libraries_unverified"}.issubset(
        gate["failure_reasons"])


def test_ros2_preflight_schema_rejects_false_available_claim():
    value = _ros2_preflight_value(tracing_available=True)
    value["ros2_tracing"]["required_tracepoints"] = []
    assert "ros2_tracing.available_condition" in core.validate_ros2_preflight_schema(value)


def test_ros2_config_schema_has_p0a_and_tracing_conditions():
    schema = json.loads((Path(core.__file__).parent / "schemas/config.schema.v1.json").read_text())
    ros2 = schema["properties"]["adapters"]["properties"]["ros2"]
    assert ros2["properties"]["ros2_prefix"] == {"type": "string", "pattern": "^/"}
    assert ros2["allOf"][1]["then"]["required"] == ["required_tracepoints"]
    required = schema["allOf"][0]["then"]["properties"]["adapters"]["properties"]["ros2"]["required"]
    assert required == ["required_runtime_libraries"]
    versions = schema["properties"]["p0a_evidence"]["properties"][
        "registration_contract_version"]["enum"]
    assert versions[-3:] == ["mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8",
                             "mw_perf.p0a_contract.v9"]
    expectations = schema["properties"]["p0a_evidence"]["properties"][
        "delivery_expectations"]
    assert expectations["additionalProperties"] is False
    assert expectations["required"] == [
        "publish_count", "readers_per_sample", "ordering_policy"]
    p0a = schema["properties"]["p0a_evidence"]
    assert p0a["properties"]["observer"] == {"type": "boolean"}
    assert "observer_latency" in p0a["properties"]["measurement_kind"]["enum"]
    assert len(p0a["allOf"]) == 3


def test_observer_latency_is_in_result_and_core_manifest_vocabularies():
    schema = json.loads((Path(core.__file__).parent / "schemas/result.schema.v1.json").read_text())
    assert "observer_latency" in schema["properties"]["measurement_kind"]["enum"]
    manifest = {"measurement_kind": "observer_latency"}
    assert "measurement_kind" not in core._manifest_schema_errors(manifest)


@pytest.mark.parametrize("adapter,p0a", [
    ("ibmw", {"run_type": "observer_diagnostic", "observer": True,
              "measurement_kind": "observer_latency"}),
    ("ros2", {"run_type": "observer_diagnostic", "observer": False,
              "measurement_kind": "observer_latency"}),
    ("ros2", {"run_type": "observer_diagnostic", "observer": True,
              "measurement_kind": "exact_correlated"}),
    ("ros2", {"run_type": "observer_diagnostic", "observer": True}),
    ("ibmw", {"run_type": "exact_run", "observer": True,
              "measurement_kind": "exact_correlated"}),
    ("ibmw", {"run_type": "exact_run", "observer": False,
              "measurement_kind": "stage_only"}),
    ("ibmw", {"run_type": "stage_run", "observer": True,
              "measurement_kind": "stage_only"}),
    ("ibmw", {"run_type": "stage_run", "observer": False,
              "measurement_kind": "observer_latency"}),
])
def test_p0a_control_plane_admission_rejects_invalid_combinations(adapter, p0a):
    with pytest.raises(ValueError):
        core._run_admission(adapter, p0a)


def test_stage_and_exact_control_plane_admission_remain_enabled():
    assert core._run_admission("ibmw", {"run_type": "stage_run", "observer": False,
        "measurement_kind": "stage_only"}) is None
    assert core._run_admission("ibmw", {"run_type": "exact_run", "observer": False,
        "measurement_kind": "exact_correlated"}) is None


def test_ros2_observer_diagnostic_refuses_before_launch_or_output(
        tmp_path, monkeypatch, capsys):
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "sut": {"mode": "managed", "command": ["/bin/true"], "env": {
            "MW_PERF_COMM_TRACE_DIR_FD": "forged", "IBMW_COMM_TRACE": "full"}},
        "p0a_evidence": {"run_type": "observer_diagnostic", "observer": True,
                         "measurement_kind": "observer_latency"},
    }))
    output = tmp_path / "runs"
    monkeypatch.setattr(core.subprocess, "Popen",
                        lambda *_args, **_kwargs: pytest.fail("SUT was launched"))
    args = core.argparse.Namespace(config=str(config), adapter="ros2", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_INVALID
    refusal = json.loads(capsys.readouterr().out)
    assert refusal == {
        "failure_code": "ROS2_OBSERVER_NOT_IMPLEMENTED",
        "failure_codes": ["ROS2_OBSERVER_NOT_IMPLEMENTED"],
        "metric_availability": "unsupported", "required_stop": True,
        "status": "refused", "valid_for_comparison": False,
        "valid_for_metric": False,
    }
    assert not output.exists()


def test_validation_instrumented_deployment_mode_stops_before_sample_on_preflight_failure(
        tmp_path, monkeypatch):
    def collect(*args, **kwargs):
        return {"status": "ok", "readiness": {"ok": True}, "observations": [
            {"name": "ros2_preflight", "value": _ros2_preflight_value(
                identity_valid=False, libraries_verified=False)}]}

    monkeypatch.setattr(core, "_collect_adapter", collect)
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "sut": {"mode": "managed", "command": ["/bin/sleep", "5"]},
        "adapters": {"ros2": {"deployment_mode": "validation_instrumented",
            "ros2_executable": "/bin/true", "required_runtime_libraries": [
                {"id": "fastdds", "name": "libfastdds.so", "sha256": "0" * 64}]}},
    }))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="ros2", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_INVALID
    run_dir = next(output.iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["run_status"] == "invalid"
    assert manifest["required_stop"] is True
    assert "SAMPLE" not in manifest["states"]
    assert manifest["sample_count"] == 0


def test_effective_validation_profile_matrix_and_conflicts():
    cases = [
        ({}, "standard"),
        ({"p0a_evidence": {}}, "p0a"),
        ({"p0a_evidence": {"run_type": "p0a_proof"}}, "p0a"),
        ({"p0a_evidence": {"run_type": "exact_run"}}, "p0a"),
        ({"adapters": {"ros2": {"validation_instrumented": True}}}, "p0a"),
        ({"adapters": {"ros2": {"validation_instrumented": False}}}, "standard"),
    ]
    for config, expected in cases:
        assert core.effective_validation_profile(config) == expected
    for config in (
        {"validation_profile": "standard", "p0a_evidence": {}},
        {"validation_profile": "p0a"},
        {"p0a_evidence": {}, "adapters": {"ibmw": {"p0a_evidence": {"run_type": "exact_run"}}}},
        {"adapters": {"ros2": {"validation_instrumented": "true"}}},
    ):
        with pytest.raises(ValueError):
            core.effective_validation_profile(config)


def test_effective_profile_from_ros2_deployment_mode():
    config = {"adapters": {"ros2": {"deployment_mode": "validation_instrumented"}}}
    assert core.effective_validation_profile(config) == "p0a"


def test_strict_p0a_json_rejects_duplicate_identity_finalized_and_conclusion(tmp_path):
    for field in ("run_id", "finalized", "conclusion"):
        path = tmp_path / field
        manifest = _build_p0a(path)
        artifact = path / "capability/resolution.json"
        text = artifact.read_text()
        marker = text.rfind("}")
        value = json.loads(text)[field]
        artifact.write_text(text[:marker] + f', "{field}": {json.dumps(value)}' + text[marker:])
        assert "P0A_ARTIFACT" in validate_p0a_evidence_manifest(path, manifest)["failure_codes"]


def test_runtime_observations_keep_exe_source_positive_case():
    observations = core._runtime_observations(os.getpid(), "run")
    executable = str(Path(f"/proc/{os.getpid()}/exe").resolve(strict=True))
    assert any(item["source"] == "exe" and item["resolved_path"] == executable
               and item["identity_source"] == "proc_exe" for item in observations)


def test_runtime_image_config_schema_requires_nonempty_declaration_strings():
    schema = json.loads((Path(core.__file__).parent/"schemas/config.schema.v1.json").read_text())
    provenance = schema["properties"]["adapters"]["properties"]["ibmw"]["properties"]["provenance"]
    declaration = provenance["properties"]["runtime_images"]["items"]
    assert provenance["allOf"][0]["then"]["required"] == ["runtime_images"]
    assert {"role", "name", "source", "expected_path"}.issubset(declaration["required"])
    for field in ("role", "name", "expected_path"):
        assert declaration["properties"][field]["minLength"] == 1
    assert declaration["properties"]["source"]["type"] == "string"


def test_result_schema_requires_boolean_validation_outcomes():
    schema = json.loads((Path(core.__file__).parent/"schemas/result.schema.v1.json").read_text())
    assert schema["properties"]["metrics_valid"] == {"type": "boolean"}
    assert schema["properties"]["adapter_readiness"] == {"type": "boolean"}
    assert {"metrics_valid", "adapter_readiness"}.issubset(schema["required"])


def test_result_schema_requires_sample_artifact_evidence():
    schema = json.loads((Path(core.__file__).parent/"schemas/result.schema.v1.json").read_text())
    artifact = schema["$defs"]["sampleArtifact"]
    samples = schema["properties"]["sample_artifacts"]
    assert "sample_artifacts" in schema["required"]
    assert set(samples["required"]) == {"process", "network"}
    assert set(artifact["required"]) == {"relative_path", "sha256", "record_count"}
    assert artifact["properties"]["sha256"] == {"$ref": "#/$defs/sha256"}
    assert artifact["properties"]["record_count"] == {"type": "integer", "minimum": 0}


def test_p0a_resolution_declarations_are_normalized_and_reject_ambiguity(tmp_path):
    first = tmp_path / "first"; first.write_bytes(b"first")
    second = tmp_path / "second"; second.write_bytes(b"second")
    values = [
        {"role": "z", "name": "second", "source": "maps", "expected_path": str(second),
         "sha256": core.hashlib.sha256(second.read_bytes()).hexdigest()},
        {"role": "a", "name": "first", "source": "exe", "expected_path": str(first),
         "sha256": core.hashlib.sha256(first.read_bytes()).hexdigest()},
    ]
    normalized = core._normalize_resolution_declarations(values)
    assert [item["role"] for item in normalized] == ["a", "z"]
    first_identity = next(item for item in normalized if item["name"] == "first")
    info = first.stat()
    assert (first_identity["expected_device"], first_identity["expected_inode"],
            first_identity["expected_size"]) == (info.st_dev, info.st_ino, info.st_size)
    assert core.hashlib.sha256(core._canonical_json_bytes(normalized)).hexdigest() == \
        core.hashlib.sha256(core._canonical_json_bytes(
            core._normalize_resolution_declarations(list(reversed(values))))).hexdigest()
    duplicate = [values[0], {**values[1], "role": values[0]["role"],
                             "name": values[0]["name"]}]
    with pytest.raises(ValueError):
        core._normalize_resolution_declarations(duplicate)
    with pytest.raises(ValueError):
        core._normalize_resolution_declarations([{**values[0], "extra": True}])
    with pytest.raises(ValueError, match="SHA-256"):
        core._normalize_resolution_declarations([{**values[0], "sha256": "0" * 64}])


def test_fd_identity_rejects_same_size_timestamp_change(tmp_path, monkeypatch):
    image = tmp_path / "image"
    image.write_bytes(b"fixed-size")
    fd = os.open(image, os.O_RDONLY)
    real_fstat = os.fstat
    before = real_fstat(fd)
    class ChangedStat:
        st_dev = before.st_dev
        st_ino = before.st_ino
        st_size = before.st_size
        st_mtime_ns = before.st_mtime_ns + 1
        st_ctime_ns = before.st_ctime_ns

    changed = ChangedStat()
    calls = iter((before, changed))
    monkeypatch.setattr(core.os, "fstat", lambda _fd: next(calls))
    try:
        with pytest.raises(OSError, match="changed while hashing"):
            core._fd_identity(fd, str(image))
    finally:
        os.close(fd)


def _build_v3_resolution_fixture(path, monkeypatch):
    original_declarations = core._p0a_declarations

    def proof_only(contract, run_type, observer, measurement_kind, deployment_mode):
        declarations = original_declarations(
            contract, run_type, observer, measurement_kind, deployment_mode)
        if contract["registration_contract_version"] == "mw_perf.p0a_contract.v3":
            declarations = [item for item in declarations
                            if not item["relative_path"].startswith("raw/comm_trace/")
                            and item["relative_path"] not in (
                                "normalized/comm_events.jsonl", "normalized/latency_samples.jsonl",
                                "capability/communication_latency.json",
                                "validation/communication_delivery.json")]
        return declarations

    monkeypatch.setattr(core, "_p0a_declarations", proof_only)
    path.mkdir()
    _write_p0a_artifacts(path, "p0a_proof")
    device = os.makedev(8, 1)
    declarations = [
        {"role": "extension", "name": "plugin", "source": "maps",
         "expected_path": "/offline/plugin.so", "sha256": "b" * 64,
         "expected_device": device, "expected_inode": 11, "expected_size": 21},
        {"role": "sut", "name": "candidate", "source": "exe",
         "expected_path": "/offline/candidate", "sha256": "a" * 64,
         "expected_device": device, "expected_inode": 10, "expected_size": 20},
    ]
    observations = [
        {"run_id": "r", "pid": 101, "start_time_ticks": 202, "source": "exe",
         "resolved_path": "/offline/candidate", "device": device, "inode": 10,
         "size": 20, "sha256": "a" * 64, "identity_source": "proc_exe"},
        {"run_id": "r", "pid": 101, "start_time_ticks": 202, "source": "maps",
         "resolved_path": "/offline/plugin.so", "device": device, "inode": 11,
         "size": 21, "sha256": "b" * 64, "identity_source": "proc_map_file",
         "mapping": {"start_address": "1000", "end_address": "2000",
                     "permissions": "r-xp", "device": "08:01", "inode": 11,
                     "deleted": False}},
    ]
    for observation in observations:
        observation["observation_id"] = core._observation_id(observation)
    observations.sort(key=lambda item: item["observation_id"])
    resolution = {"schema": "mw_perf.p0a.resolution.v2", "run_id": "r",
        "capability_id": "c", "path_id": "p", "deployment_mode": "managed",
        "finalized": True, "resolved": True, "conclusion": "pass",
        "producer": "mw_perf_collect",
        "declaration_sha256": core.hashlib.sha256(
            core._canonical_json_bytes(declarations)).hexdigest(),
        "declarations": declarations, "observations": observations,
        "matches": core._resolution_matches(declarations, observations)}
    resolution_bytes = core._canonical_json_bytes(resolution, newline=True)
    (path / "capability/resolution.json").write_bytes(resolution_bytes)
    (path / "raw").mkdir(exist_ok=True)
    (path / "raw/process.jsonl").write_text(json.dumps({"timestamp": 1.0,
        "processes": [{"pid": 101, "start_time_ticks": 202}]}) + "\n")
    root = {"run_id": "r", "mode": "managed", "run_type": "exact_run",
            "run_status": "completed", "pid": 101, "pid_start_time_ticks": 202,
            "p0a_resolution": {"collector_written": True,
                "relative_path": "capability/resolution.json",
                "content_sha256": core.hashlib.sha256(resolution_bytes).hexdigest(),
                "declaration_sha256": resolution["declaration_sha256"]}}
    (path / "manifest.json").write_text(json.dumps(root))
    manifest = build_p0a_evidence_manifest(path,
        {"registration_contract_version": "mw_perf.p0a_contract.v3"},
        run_id="r", capability_id="c", path_id="p", deployment_mode="managed",
        run_type="exact_run", observer=False, measurement_kind="exact_correlated")
    assert manifest is not None
    return manifest


def _sync_v3_resolution_digest(path, manifest):
    data = (path / "capability/resolution.json").read_bytes()
    digest = core.hashlib.sha256(data).hexdigest()
    for group in (manifest["entries"], manifest["required"]):
        next(item for item in group if item["relative_path"] ==
             "capability/resolution.json")["sha256"] = digest
    root = json.loads((path / "manifest.json").read_text())
    root["p0a_resolution"]["content_sha256"] = digest
    (path / "manifest.json").write_text(json.dumps(root))


def test_p0a_v3_resolution_validates_offline_and_rejects_tampering(tmp_path, monkeypatch):
    success = tmp_path / "success"
    manifest = _build_v3_resolution_fixture(success, monkeypatch)
    assert validate_p0a_evidence_manifest(success, manifest)["valid"]

    mutations = {
        "declaration_hash": lambda value: value.update(declaration_sha256="0" * 64),
        "observation_id": lambda value: value["observations"][0].update(observation_id="0" * 64),
        "observation_pid": lambda value: value["observations"][0].update(pid=999),
        "runtime_device": lambda value: value["observations"][0].update(device=999),
        "runtime_inode": lambda value: value["observations"][0].update(inode=999),
        "runtime_size": lambda value: value["observations"][0].update(size=999),
        "map_permission": lambda value: value["observations"][0]["mapping"].update(
            permissions="r--p"),
        "match_set": lambda value: value["matches"][0].update(
            observed=value["matches"][1]["observed"]),
    }
    for name, mutate in mutations.items():
        case = tmp_path / name
        changed = _build_v3_resolution_fixture(case, monkeypatch)
        artifact = case / "capability/resolution.json"
        value = json.loads(artifact.read_text())
        if name == "map_permission":
            map_index = next(index for index, item in enumerate(value["observations"])
                             if item["source"] == "maps")
            value["observations"][0], value["observations"][map_index] = \
                value["observations"][map_index], value["observations"][0]
        mutate(value)
        if name.startswith("runtime_"):
            value["observations"][0]["observation_id"] = core._observation_id(
                value["observations"][0])
            value["observations"].sort(key=lambda item: item["observation_id"])
        artifact.write_bytes(core._canonical_json_bytes(value, newline=True))
        _sync_v3_resolution_digest(case, changed)
        assert "P0A_RESOLUTION" in validate_p0a_evidence_manifest(
            case, changed)["failure_codes"], name

    publication = tmp_path / "publication"
    changed = _build_v3_resolution_fixture(publication, monkeypatch)
    root = json.loads((publication / "manifest.json").read_text())
    root["p0a_resolution"]["collector_written"] = False
    (publication / "manifest.json").write_text(json.dumps(root))
    assert "P0A_RESOLUTION" in validate_p0a_evidence_manifest(
        publication, changed)["failure_codes"]


def test_p0a_resolution_publication_is_exclusive(tmp_path):
    core._secure_write_new(tmp_path, "capability/resolution.json", b"first\n")
    with pytest.raises(FileExistsError):
        core._secure_write_new(tmp_path, "capability/resolution.json", b"second\n")


def test_p0a_resolution_publication_conflict_does_not_add_manifest_metadata(tmp_path):
    core._secure_write_new(tmp_path, "capability/resolution.json", b"preexisting\n")
    manifest = {}
    proof = {"declaration_sha256": "a" * 64}
    with pytest.raises(FileExistsError):
        core._publish_resolution(tmp_path, manifest, proof, b"new\n")
    assert "p0a_resolution" not in manifest


def test_p0a_identity_publication_conflict_does_not_add_manifest_metadata(tmp_path):
    core._secure_write_new(tmp_path, "normalized/identity_trace.jsonl", b"preexisting\n")
    manifest = {}
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
                  "source_set_sha256": "b" * 64, "record_count": 1}
    with pytest.raises(FileExistsError):
        core._publish_identity_trace(tmp_path, manifest, derivation)
    assert "p0a_identity_trace" not in manifest


def test_p0a_hook_publication_conflict_does_not_add_manifest_metadata(tmp_path):
    core._secure_write_new(tmp_path, "capability/hook_callgraph.json", b"preexisting\n")
    manifest = {}
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
                  "boundary_catalog_sha256": "b" * 64,
                  "delivery_set_sha256": "c" * 64, "record_count": 1}
    with pytest.raises(FileExistsError):
        core._publish_hook_callgraph(tmp_path, manifest, derivation)
    assert "p0a_hook_callgraph" not in manifest


@pytest.mark.parametrize("name,relative", [
    ("p0a_comm_events", "normalized/comm_events.jsonl"),
    ("p0a_latency_samples", "normalized/latency_samples.jsonl"),
])
def test_p0a_normalized_publication_conflict_does_not_add_metadata(
        tmp_path, name, relative):
    core._secure_write_new(tmp_path, relative, b"preexisting\n")
    manifest = {}
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
        "source_identity_sha256": "b" * 64,
        "source_identity_record_count": 1, "record_count": 1}
    with pytest.raises(FileExistsError):
        core._publish_normalized_result(tmp_path, manifest, name=name,
                                        relative=relative, derivation=derivation)
    assert name not in manifest


def test_p0a_normalized_second_publication_conflict_leaves_partial_run_detectable(
        tmp_path):
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
        "source_identity_sha256": "b" * 64,
        "source_identity_record_count": 1, "record_count": 1}
    manifest = {}
    core._publish_normalized_result(tmp_path, manifest, name="p0a_comm_events",
        relative="normalized/comm_events.jsonl", derivation=derivation)
    core._secure_write_new(tmp_path, "normalized/latency_samples.jsonl", b"preexisting\n")
    with pytest.raises(FileExistsError):
        core._publish_normalized_result(tmp_path, manifest, name="p0a_latency_samples",
            relative="normalized/latency_samples.jsonl", derivation=derivation)
    assert manifest["p0a_comm_events"]["collector_written"] is True
    assert "p0a_latency_samples" not in manifest


def test_p0a_communication_latency_publication_conflict_adds_no_metadata(tmp_path):
    core._secure_write_new(tmp_path, "capability/communication_latency.json", b"preexisting\n")
    manifest = {}
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
        "latency_samples_content_sha256": "b" * 64,
        "latency_samples_record_count": 1, "sample_set_sha256": "c" * 64,
        "sample_set_count": 1}
    with pytest.raises(FileExistsError):
        core._publish_communication_latency(tmp_path, manifest, derivation)
    assert "p0a_communication_latency" not in manifest


def test_p0a_communication_delivery_publication_conflict_adds_no_metadata(tmp_path):
    core._secure_write_new(
        tmp_path, "validation/communication_delivery.json", b"preexisting\n")
    manifest = {}
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
        "expected_publish_count": 1000, "expected_readers_per_sample": 1,
        "observed_publish_count": 1000, "observed_receive_count": 1000,
        "observed_dispatch_count": 1000, "observed_callback_count": 1000,
        "sample_set_sha256": "b" * 64, "delivery_set_sha256": "c" * 64,
        "record_count": 1}
    with pytest.raises(FileExistsError):
        core._publish_communication_delivery(tmp_path, manifest, derivation)
    assert "p0a_communication_delivery" not in manifest


def test_p0a_trace_quality_publication_conflict_adds_no_metadata(tmp_path):
    core._secure_write_new(tmp_path, "validation/trace_quality.json", b"preexisting\n")
    manifest = {}
    derivation = {"bytes": b"new\n", "content_sha256": "a" * 64,
        "raw_source_set_sha256": "b" * 64, "stage_record_count": 3000,
        "exact_record_count": 1000, "sample_set_sha256": "c" * 64,
        "delivery_set_sha256": "d" * 64, "record_count": 1}
    with pytest.raises(FileExistsError):
        core._publish_trace_quality(tmp_path, manifest, derivation)
    assert "p0a_trace_quality" not in manifest


@pytest.mark.parametrize("mode,run_type,include_resolution", [
    ("managed", "stage_run", True),
    ("attached", "exact_run", True),
    ("managed", "exact_run", False),
])
def test_p0a_contract_v4_rejects_ineligible_run_before_output(
        tmp_path, capsys, mode, run_type, include_resolution):
    p0a = {"registration_contract_version": "mw_perf.p0a_contract.v4",
           "run_type": run_type, "observer": False,
           "measurement_kind": "stage_only" if run_type == "stage_run"
               else "exact_correlated"}
    if include_resolution:
        p0a["resolution"] = {"runtime_images": []}
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "sut": {"mode": mode, "command": ["/bin/true"], "pid": os.getpid()},
        "p0a_evidence": p0a,
    }))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_FAILED
    assert "contract v4 requires managed exact_run with resolution" in capsys.readouterr().err
    assert not output.exists()


@pytest.mark.parametrize("changes", [
    {"run_type": "stage_run"},
    {"trace_mode": "sampled"},
    {"capability_id": "other"},
    {"measurement_kind": "stage_only"},
])
@pytest.mark.parametrize("version", [
    "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
    "mw_perf.p0a_contract.v7"])
def test_p0a_contract_v5_rejects_ineligible_run_before_output(
        tmp_path, changes, version):
    image = tmp_path / "image"; image.write_bytes(b"image")
    p0a = {"registration_contract_version": version,
           "run_type": "exact_run", "observer": False, "trace_mode": "full",
           "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
           "measurement_kind": "exact_correlated",
           "resolution": {"runtime_images": [{"role": "sut", "name": "image",
               "source": "exe", "expected_path": str(image),
               "sha256": core.hashlib.sha256(image.read_bytes()).hexdigest()}]}}
    p0a.update(changes)
    config = tmp_path / "config.json"
    config.write_text(json.dumps({"sut": {"mode": "managed", "command": ["/bin/true"]},
                                  "p0a_evidence": p0a}))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_FAILED
    assert not output.exists()


def test_p0a_contract_v5_is_trusted_and_overrides_only_hook():
    v4, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v4")
    v5, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v5")
    by_path_v4 = {item["relative_path"]: item for item in v4["artifacts"]}
    by_path_v5 = {item["relative_path"]: item for item in v5["artifacts"]}
    assert set(by_path_v5) == set(by_path_v4)
    assert by_path_v5["capability/hook_callgraph.json"]["schema_version"] == \
        "mw_perf.p0a.hook_callgraph.v2"
    for path in set(by_path_v4) - {"capability/hook_callgraph.json"}:
        assert by_path_v5[path] == by_path_v4[path]


def test_p0a_contract_v6_overrides_only_normalized_results():
    v5, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v5")
    v6, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v6")
    by_path_v5 = {item["relative_path"]: item for item in v5["artifacts"]}
    by_path_v6 = {item["relative_path"]: item for item in v6["artifacts"]}
    overridden = {"normalized/comm_events.jsonl", "normalized/latency_samples.jsonl"}
    assert set(by_path_v6) == set(by_path_v5)
    assert by_path_v6["normalized/comm_events.jsonl"]["schema_version"] == \
        "mw_perf.comm_event.v2"
    assert by_path_v6["normalized/latency_samples.jsonl"]["schema_version"] == \
        "mw_perf.latency_sample.v2"
    for path in set(by_path_v5) - overridden:
        assert by_path_v6[path] == by_path_v5[path]


def test_p0a_contract_v7_overrides_only_communication_latency():
    v6, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v6")
    v7, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v7")
    by_path_v6 = {item["relative_path"]: item for item in v6["artifacts"]}
    by_path_v7 = {item["relative_path"]: item for item in v7["artifacts"]}
    assert set(by_path_v7) == set(by_path_v6)
    assert by_path_v7["capability/communication_latency.json"]["schema_version"] == \
        "mw_perf.communication_latency.v2"
    for path in set(by_path_v6) - {"capability/communication_latency.json"}:
        assert by_path_v7[path] == by_path_v6[path]
    schema = v7["artifact_schemas"]["mw_perf.communication_latency.v2"]
    def assert_objects_closed(item):
        if isinstance(item, dict):
            if item.get("type") == "object":
                assert item.get("additionalProperties") is False
            for child in item.values():
                assert_objects_closed(child)
        elif isinstance(item, list):
            for child in item:
                assert_objects_closed(child)
    assert_objects_closed(schema)
    assert "stats" not in json.dumps(schema)


def test_p0a_contract_v8_inherits_v7_and_adds_only_trace_quality():
    v7, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v7")
    v8, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    by_path_v7 = {item["relative_path"]: item for item in v7["artifacts"]}
    by_path_v8 = {item["relative_path"]: item for item in v8["artifacts"]}
    assert set(by_path_v8) == set(by_path_v7) | {"validation/trace_quality.json"}
    for path in set(by_path_v7) - {"validation/communication_delivery.json"}:
        assert by_path_v8[path] == by_path_v7[path]
    delivery = by_path_v8["validation/communication_delivery.json"]
    assert delivery["schema_version"] == "mw_perf.communication_delivery.v2"
    schema = v8["artifact_schemas"]["mw_perf.communication_delivery.v2"]
    trace_quality = by_path_v8["validation/trace_quality.json"]
    assert trace_quality["schema_version"] == "mw_perf.trace_quality.v1"
    trace_schema = v8["artifact_schemas"]["mw_perf.trace_quality.v1"]

    def assert_objects_closed(item):
        if isinstance(item, dict):
            if item.get("type") == "object":
                assert item.get("additionalProperties") is False
            for child in item.values():
                assert_objects_closed(child)
        elif isinstance(item, list):
            for child in item:
                assert_objects_closed(child)

    assert_objects_closed(schema)
    assert_objects_closed(trace_schema)


def test_p0a_contract_v9_inherits_v8_and_adds_formal_anchor_artifacts():
    v8, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    v9, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v9")
    by_path_v8 = {item["relative_path"]: item for item in v8["artifacts"]}
    by_path_v9 = {item["relative_path"]: item for item in v9["artifacts"]}
    assert set(by_path_v9) == set(by_path_v8) | {"validation/formal_anchor.json"}
    for path in set(by_path_v8) - {"validation/trace_quality.json", "validation/fault_injection.json", "validation/rollback.json"}:
        assert by_path_v9[path] == by_path_v8[path]
    assert v9["run_types"]["formal_p0a"] == {"universes": ["proof", "collection", "observer", "formal_anchor"]}
    assert by_path_v9["validation/trace_quality.json"]["schema_version"] == "mw_perf.trace_quality_formal.v2"
    assert by_path_v9["validation/fault_injection.json"]["schema_version"] == "mw_perf.fault_injection_formal.v2"
    assert by_path_v9["validation/rollback.json"]["schema_version"] == "mw_perf.rollback_formal.v2"
    assert v9["registration_contract_version"] == "mw_perf.p0a_contract.v9"
    stored_v9 = json.loads((Path(core.__file__).parent /
        "contracts/p0a-evidence-contract.v9.json").read_text())
    assert stored_v9["base_contract_version"] == "mw_perf.p0a_contract.v8"

    def assert_objects_closed(item):
        if isinstance(item, dict):
            if item.get("type") == "object":
                assert item.get("additionalProperties") is False
            for child in item.values():
                assert_objects_closed(child)
        elif isinstance(item, list):
            for child in item:
                assert_objects_closed(child)

    for schema_id in ("mw_perf.formal_anchor.v1", "mw_perf.trace_quality_formal.v2",
                      "mw_perf.fault_injection_formal.v2", "mw_perf.rollback_formal.v2"):
        assert_objects_closed(v9["artifact_schemas"][schema_id])
    assert v8["artifact_schemas"].get("mw_perf.formal_anchor.v1") is None


@pytest.mark.parametrize("schema_id", [
    "mw_perf.formal_anchor.v1", "mw_perf.trace_quality_formal.v2",
    "mw_perf.fault_injection_formal.v2", "mw_perf.rollback_formal.v2"])
def test_p0a_contract_v9_formal_schemas_keep_statuses_separate(schema_id):
    contract, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v9")
    schema = contract["artifact_schemas"][schema_id]
    descriptor = {"campaign_id": "campaign", "role": "exact_anchor", "companion_id": "anchor",
                  "object_sha256": "0" * 64, "bundle_manifest_sha256": "0" * 64,
                  "archive_tree_sha256": "0" * 64, "run_id": "run",
                  "evidence_contract": {"id": "v8", "sha256": "0" * 64},
                  "root_manifest_sha256": "0" * 64, "evidence_manifest_sha256": "0" * 64,
                  "build_identity_sha256": "0" * 64}
    value = {"schema": schema_id, "run_id": "run", "finalized": True,
             "status": {"valid_for_p0a_completion": True,
                         "metric_availability": "unsupported",
                         "valid_for_comparison": False}, "conclusion": "pass"}
    if schema_id == "mw_perf.formal_anchor.v1":
        value["formal_anchor_id"] = "0" * 64
        value["registration_contract"] = {"version": "mw_perf.p0a_contract.v9", "sha256": "0" * 64}
        value["source_anchor"] = descriptor
        value["companions"] = {role: dict(descriptor, role=role) for role in ("fault_evidence_tamper", "fault_runtime_matrix", "rollback_trace_off")}
        value["source_trace_quality_content_sha256"] = "0" * 64
    else:
        value["formal_anchor_sha256"] = "0" * 64
        value["source_anchor"] = descriptor
        value["source_evidence_manifest_sha256"] = "0" * 64
        if schema_id == "mw_perf.trace_quality_formal.v2":
            value["source_trace_quality_content_sha256"] = "0" * 64
        elif schema_id == "mw_perf.fault_injection_formal.v2":
            value["companions"] = {
                role: dict(descriptor, role=role)
                for role in ("fault_evidence_tamper", "fault_runtime_matrix")
            }
        else:
            value["companion"] = dict(descriptor, role="rollback_trace_off")
    assert core._schema_errors(value, schema) == []
    value["status"]["valid_for_comparison"] = True
    assert core._schema_errors(value, schema)
    value["status"]["valid_for_comparison"] = False
    value["unexpected"] = True
    assert core._schema_errors(value, schema)


@pytest.mark.parametrize("expectations", [
    None,
    {"publish_count": 999, "readers_per_sample": 1,
     "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records"},
    {"publish_count": 1000, "readers_per_sample": 2,
     "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records"},
    {"publish_count": 1000, "readers_per_sample": 1,
     "ordering_policy": "other"},
    {"publish_count": 1000, "readers_per_sample": 1,
     "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records", "extra": True},
])
def test_p0a_contract_v8_rejects_nonexact_expectations_before_output(
        tmp_path, expectations):
    images = []
    for role, source in (("sut", "exe"), ("extension", "maps")):
        path = (tmp_path / role).resolve()
        path.write_bytes(role.encode())
        images.append({"role": role, "name": role, "source": source,
            "expected_path": str(path),
            "sha256": core.hashlib.sha256(path.read_bytes()).hexdigest()})
    p0a = {"registration_contract_version": "mw_perf.p0a_contract.v8",
        "run_type": "exact_run", "trace_mode": "full", "candidate_only": True,
        "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "measurement_kind": "exact_correlated",
        "resolution": {"runtime_images": images}}
    if expectations is not None:
        p0a["delivery_expectations"] = expectations
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "sut": {"mode": "managed", "command": ["/bin/true"]},
        "p0a_evidence": p0a}))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_FAILED
    assert not output.exists()


@pytest.mark.parametrize("candidate_only", [False, None])
def test_p0a_contract_v7_requires_candidate_only_before_output(
        tmp_path, candidate_only):
    images = []
    for role, source in (("sut", "exe"), ("extension", "maps")):
        path = (tmp_path / role).resolve()
        path.write_bytes(role.encode())
        images.append({"role": role, "name": role, "source": source,
            "expected_path": str(path),
            "sha256": core.hashlib.sha256(path.read_bytes()).hexdigest()})
    p0a = {"registration_contract_version": "mw_perf.p0a_contract.v7",
        "run_type": "exact_run", "trace_mode": "full",
        "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "measurement_kind": "exact_correlated",
        "resolution": {"runtime_images": images}}
    if candidate_only is not None:
        p0a["candidate_only"] = candidate_only
    config = tmp_path / "config.json"
    config.write_text(json.dumps({"sut": {"mode": "managed", "command": ["/bin/true"]},
                                  "p0a_evidence": p0a}))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_FAILED
    assert not output.exists()


def test_p0a_contract_v1_through_v7_bytes_are_pinned():
    expected = {
        "mw_perf.p0a_contract.v1": "9bc17677e794d4341656b16af66ff121211e3bd3d4dba8d162a889c41a3e28eb",
        "mw_perf.p0a_contract.v2": "dc2da2f476bbffe4e0728455279e8841b00ae01b3bf57612f3e84aa7dedcddd5",
        "mw_perf.p0a_contract.v3": "e122d6b8dd774c5c2a458f5557624cc5fa49c9d259e9b2a604344310020c2c9d",
        "mw_perf.p0a_contract.v4": "99ba12db651bec747d9d03b46ae48a84db3015bd4e485852448c49d82b164cf2",
        "mw_perf.p0a_contract.v5": "d564dbaf47ac11101c691c66168d5708025640b563d96378c0a94e43fb8d7004",
        "mw_perf.p0a_contract.v6": "8bf1394a3744c9023365b679c6c2cf1063fbb9d143e5e0e6c1fd65176af3413e",
        "mw_perf.p0a_contract.v7": "eb3bc93735bfe6adb6bfbffa67bfd4b6d3cf51658c7bc76a1d0b64fc41e9cc29",
    }
    for version, digest in expected.items():
        _contract, raw = core._trusted_p0a_contract(version)
        assert core.hashlib.sha256(raw).hexdigest() == digest


def test_p0a_contract_v5_resolution_roles_are_admitted_fail_closed():
    eligible = [
        {"role": "sut", "source": "exe"},
        {"role": "extension", "source": "maps"},
    ]
    core._validate_v5_resolution_roles(eligible)
    for declarations in (
        eligible[:1],
        [{"role": "sut", "source": "maps"},
         {"role": "extension", "source": "exe"}],
        [{"role": "sut", "source": "exe"},
         {"role": "plugin", "source": "maps"}],
    ):
        with pytest.raises(ValueError, match="sut/exe and extension/maps"):
            core._validate_v5_resolution_roles(declarations)


@pytest.mark.parametrize("runtime_images", [
    [{"role": "sut", "name": "sut", "source": "exe"}],
    [{"role": "sut", "name": "sut", "source": "maps"},
     {"role": "extension", "name": "extension", "source": "exe"}],
])
@pytest.mark.parametrize("version", [
    "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
    "mw_perf.p0a_contract.v7"])
def test_p0a_contract_v5_rejects_resolution_roles_before_output(
        tmp_path, runtime_images, version):
    declarations = []
    for index, item in enumerate(runtime_images):
        image = tmp_path / f"image-{index}"
        image.write_bytes(f"image-{index}".encode())
        declarations.append({**item, "expected_path": str(image),
            "sha256": core.hashlib.sha256(image.read_bytes()).hexdigest()})
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "sut": {"mode": "managed", "command": ["/bin/true"]},
        "p0a_evidence": {
            "registration_contract_version": version,
            "run_type": "exact_run", "trace_mode": "full",
            "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
            "measurement_kind": "exact_correlated",
            "resolution": {"runtime_images": declarations},
        },
    }))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_FAILED
    assert not output.exists()


@pytest.mark.parametrize("candidate_only", [1, "true", None])
def test_p0a_candidate_only_requires_boolean_before_output(
        tmp_path, capsys, candidate_only):
    config = tmp_path / "config.json"
    config.write_text(json.dumps({
        "sut": {"mode": "managed", "command": ["/bin/true"]},
        "p0a_evidence": {"candidate_only": candidate_only},
    }))
    output = tmp_path / "runs"
    args = core.argparse.Namespace(config=str(config), adapter="generic", command=[],
                                   override_command=False, output=str(output))
    assert core.run(args) == core.EXIT_FAILED
    assert "candidate_only must be boolean" in capsys.readouterr().err
    assert not output.exists()


def test_result_schema_required_provenance_and_runtime_images_contract(tmp_path):
    schema = json.loads((Path(core.__file__).parent/"schemas/result.schema.v1.json").read_text())
    binary = tmp_path / "sut"
    binary.write_bytes(b"binary")
    identity = core._file_identity(binary)
    identity["declared_path"] = str(binary)
    identity["mtime_ns"] = binary.stat().st_mtime_ns
    observation = {"run_id": "test-run", "pid": 1, "start_time_ticks": 2,
                   "source": "exe", "identity_source": "proc_exe", **core._file_identity(binary)}
    observation["observation_id"] = core._observation_id(observation)
    declaration = {"role": "sut", "name": "sut", "source": "exe",
                   "expected_path": str(binary)}
    evidence = core._runtime_image_result([declaration], [observation], "test-run", [identity])
    manifest = {**_valid_manifest(), "adapter": "ibmw",
                "provenance": {"required": True, "binaries": [identity],
                               "source": {}, "build": {}},
                "runtime_images": {"content_sha256": "a" * 64, "evidence": evidence}}

    assert schema["$schema"] == "https://json-schema.org/draft/2020-12/schema"
    binary_schema = schema["$defs"]["binaryIdentity"]
    assert {"declared_path", "resolved_path", "device", "inode", "size", "mtime_ns", "sha256"} == set(binary_schema["required"])
    assert set(binary_schema["required"]).issubset(identity)
    runtime_schema = schema["properties"]["runtime_images"]
    evidence_schema = schema["$defs"]["runtimeImageEvidence"]
    assert set(runtime_schema["required"]).issubset(manifest["runtime_images"])
    assert set(evidence_schema["required"]).issubset(evidence)
    assert schema["allOf"][0]["then"]["required"] == ["runtime_images"]
    assert schema["properties"]["provenance"]["allOf"][0]["then"]["properties"]["binaries"]["minItems"] == 1
    for field in ("device", "inode"):
        invalid = json.loads(json.dumps(manifest))
        invalid["provenance"]["binaries"][0].pop(field)
        missing = set(binary_schema["required"]) - invalid["provenance"]["binaries"][0].keys()
        assert missing == {field}
    missing_runtime = json.loads(json.dumps(manifest))
    missing_runtime.pop("runtime_images")
    assert set(schema["allOf"][0]["then"]["required"]) - missing_runtime.keys() == {"runtime_images"}


def test_validation_outcomes_are_required_in_manifest(tmp_path):
    for field in ("metrics_valid", "adapter_readiness"):
        case = tmp_path/field; case.mkdir(); _write_valid_artifacts(case)
        manifest = json.loads((case/"manifest.json").read_text())
        del manifest[field]
        (case/"manifest.json").write_text(json.dumps(manifest))
        result = validate_dir(case)
        assert result["valid_for_collection"] is False
        assert f"REQUIRED_FIELD_MISSING:{field}" in result["failure_codes"]
