import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tarfile
from unittest.mock import patch

import pytest

from mw_perf_collect import core, ohos_capture, ohos_remote_p0a
from mw_perf_collect.cli import main


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")


def _native_trace(tmp_path, run_id, capability, path_id, topology_generation, topology_sha256):
    trace = tmp_path / "native-trace"
    trace.mkdir()
    fd = os.open(trace, os.O_RDONLY | os.O_DIRECTORY)
    environment = dict(os.environ)
    environment.update({
        "MW_PERF_COMM_TRACE_DIR_FD": str(fd), "MW_PERF_RUN_ID": run_id,
        "MW_PERF_CAPABILITY_ID": capability, "MW_PERF_PATH_ID": path_id,
        "MW_PERF_DEPLOYMENT_MODE": "managed",
        "MW_PERF_TOPOLOGY_GENERATION": topology_generation,
        "MW_PERF_TOPOLOGY_SHA256": topology_sha256,
        "MW_PERF_EXACT_REQUIRED": "1",
    })
    fixture = Path(__file__).parents[1] / "native/valid_trace_fixture.py"
    try:
        subprocess.run([sys.executable, str(fixture)], env=environment, pass_fds=(fd,),
                       check=True)
    finally:
        os.close(fd)
    files = _expand_native_trace({item.name: item.read_bytes() for item in trace.iterdir()})
    meta = json.loads(next(raw for name, raw in files.items()
                           if name.endswith(".meta.json") and ".exact." not in name))
    return files, meta["producer_pid"], meta["producer_start_time_ticks"]


def _expand_native_trace(files):
    stage_name = next(name for name in files if name.endswith(".ibct"))
    stage = files[stage_name]
    templates = [stage[256 + index * 64:256 + (index + 1) * 64] for index in range(3)]
    stage_records = []
    for sequence in range(1, 1001):
        base = (sequence - 1) * 100
        for template, start, end in zip(templates, (10, 20, 40), (20, 30, 70)):
            record = bytearray(template)
            struct.pack_into("<QQ", record, 0, base + start, base + end)
            struct.pack_into("<iI", record, 32, 0, sequence)
            stage_records.append(bytes(record))
    stage_payload = b"".join(stage_records)
    stage_header = bytearray(stage[:256])
    struct.pack_into("<Q", stage_header, 96, len(stage_records))
    stage_header[136:168] = hashlib.sha256(stage_payload).digest()
    files[stage_name] = bytes(stage_header) + stage_payload

    exact_name = next(name for name in files if name.endswith(".ibce"))
    exact = files[exact_name]
    exact_template = exact[160:232]
    exact_records = []
    for sequence in range(1, 1001):
        record = bytearray(exact_template)
        base = (sequence - 1) * 100
        struct.pack_into("<QQ", record, 0, base + 50, base + 60)
        struct.pack_into("<iI", record, 32, 0, sequence)
        exact_records.append(bytes(record))
    exact_payload = b"".join(exact_records)
    exact_header = bytearray(exact[:160])
    struct.pack_into("<Q", exact_header, 80, 1000)
    exact_header[128:160] = hashlib.sha256(exact_payload).digest()
    files[exact_name] = bytes(exact_header) + exact_payload
    for name in [item for item in files if item.endswith(".seal.json")]:
        seal = json.loads(files[name])
        seal["record_count"] = 1000 if ".exact." in name else 3000
        if "exported" in seal:
            seal["exported"] = 3000
        files[name] = _json(seal)
    return files


def _fixture(tmp_path):
    tmp_path.mkdir(parents=True, exist_ok=True)
    challenge = "d" * 64
    capability = "dds.ordinary.nonplain.single_driver.sync_callback"
    topology_sha256 = "c" * 64
    request = {
        "protocol": "mw_perf.ohos_remote_capture.request.v2",
        "capture_role": "exact_anchor", "campaign_id": "campaign-1", "fault": "none",
        "workload_id": capability,
        "registration_contract": {"id": "mw_perf.p0a_contract.v8",
            "sha256": core._RESOURCE_SHA256["contracts/p0a-evidence-contract.v8.json"]},
        "runtime_images": [
            {"role": "sut", "name": "candidate", "source": "exe",
             "path": "/system/bin/candidate", "sha256": "a" * 64},
            {"role": "extension", "name": "dds-ext", "source": "maps",
             "path": "/system/lib64/libdds_ext.so", "sha256": "b" * 64},
        ],
        "ready_timeout_ms": 1000, "completion_timeout_ms": 1000,
        "argv": ["/system/bin/candidate"], "capability_id": capability, "env": [],
        "exact_required": True, "path_id": "path-1", "run_id": "run-1",
        "run_root": "/data/capture", "timeout_ms": 5000,
        "topology_generation": "gen-1", "topology_sha256": topology_sha256,
        "trace_mode": "full", "request_challenge": challenge,
    }
    files, pid, ticks = _native_trace(
        tmp_path, request["run_id"], capability, request["path_id"],
        request["topology_generation"], topology_sha256)
    identity = {"request_challenge": challenge, "campaign_id": "campaign-1",
                "run_id": "run-1", "capture_role": "exact_anchor", "boot_id": "boot-1",
                "child_pid": pid, "child_start_time_ticks": ticks}
    live = [{"role": image["role"], "name": image["name"], "source": image["source"],
             "expected_path": image["path"], "expected_sha256": image["sha256"],
             "observed_sha256": image["sha256"], "observed_device_major": 8,
             "observed_device_minor": index + 1, "observed_inode": index + 10,
             "visible": True, "matched": True}
            for index, image in enumerate(request["runtime_images"])]
    images_hash = hashlib.sha256(_json(live)).hexdigest()
    completion = identity | {
        "protocol": "mw_perf.ohos_device_completion.v2", "ready": True,
        "released": True, "complete": True, "completion_images": live,
        "ready_attested": True, "completion_attested": True, "evidence_released": True,
        "completion_eof_observed": True, "ready_images_sha256": images_hash,
        "completion_images_sha256": images_hash,
    }
    attestation = identity | {
        "protocol": "mw_perf.ohos_runtime_attestation.v2",
        "prelaunch_images": request["runtime_images"], "live_images": live,
        "completion_images": live, "ready_attested": True, "completion_attested": True,
        "evidence_released": True, "completion_eof_observed": True,
        "ready_images_sha256": images_hash, "completion_images_sha256": images_hash,
    }
    files.update({"device_completion.json": _json(completion),
                  "device_runtime_attestation.json": _json(attestation)})
    source = [{"name": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
              for name, raw in sorted(files.items())]
    tree = hashlib.sha256()
    for item in source:
        tree.update(item["name"].encode() + b"\0" + str(item["size"]).encode() + b"\0"
                    + item["sha256"].encode() + b"\n")
    manifest = {
        "protocol": "mw_perf.ohos_remote_capture.v2", "nonce": "e" * 32,
        "request_challenge": challenge, "run_id": "run-1", "capability_id": capability,
        "path_id": "path-1", "topology_generation": "gen-1",
        "topology_sha256": topology_sha256, "trace_mode": "full", "exact_required": True,
        "boot_id": "boot-1", "agent_executable_sha256": "f" * 64,
        "request_sha256": hashlib.sha256(ohos_capture._canonical_request(request)).hexdigest(),
        "child_pid": pid, "child_start_time_ticks": ticks, "wait_status": 0,
        "graceful": True, "no_escalation": True, "descendant_policy": "forbid",
        "descendant_detected": False, "semantic_validation": False,
        "source_files": source, "archive_tree_sha256": tree.hexdigest(),
        "snapshot_valid": True, "reasons": [], "capture_role": "exact_anchor",
        "campaign_id": "campaign-1", "fault": "none", "workload_id": capability,
        "registration_contract": request["registration_contract"],
        "runtime_images": request["runtime_images"], "ready_observed": True,
        "release_granted": True, "completion_observed": True,
        "completion_images": live, "completion_attested": True, "evidence_released": True,
        "ready_attested": True, "completion_eof_observed": True,
        "ready_images_sha256": images_hash, "completion_images_sha256": images_hash,
        "device_completion_sha256": hashlib.sha256(files["device_completion.json"]).hexdigest(),
        "device_runtime_attestation_sha256": hashlib.sha256(
            files["device_runtime_attestation.json"]).hexdigest(),
    }
    archive = tmp_path / f"capture-{challenge}.tar"
    with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as stream:
        for name, raw in [("device_capture_manifest.json", _json(manifest)),
                          *sorted(files.items())]:
            info = tarfile.TarInfo(name)
            info.size = len(raw); info.mode = 0o600; info.uid = info.gid = info.mtime = 0
            info.type = tarfile.REGTYPE
            stream.addfile(info, io.BytesIO(raw))
    tar_raw = archive.read_bytes()
    used = ((len(tar_raw.rstrip(b"\0")) + 511) // 512) * 512
    archive.write_bytes(tar_raw[:used] + b"\0" * 1024)
    os.chmod(archive, 0o600)
    archive_raw = archive.read_bytes()
    result = {"protocol": "mw_perf.ohos_remote_capture.result.v2",
              "snapshot_valid": True, "semantic_validation": False,
              "archive_path": f"/data/capture/{archive.name}",
              "request_challenge": challenge,
              "archive_sha256": hashlib.sha256(archive_raw).hexdigest(),
              "archive_size": len(archive_raw), "child_pid": pid,
              "child_start_time_ticks": ticks, "reasons": []}
    result_raw, request_raw = _json(result) + b"\n", _json(request) + b"\n"
    return archive, result, request, result_raw, request_raw


def _materialize(fixture, destination):
    archive, result, request, result_raw, request_raw = fixture
    return ohos_remote_p0a.materialize_ohos_p0a_candidate(
        archive, result, request, destination,
        result_sha256=hashlib.sha256(result_raw).hexdigest(),
        request_sha256=hashlib.sha256(request_raw).hexdigest())


def test_materializes_complete_candidate_from_native_trace(tmp_path):
    fixture = _fixture(tmp_path / "source")
    destination = tmp_path / "candidate"
    validation = _materialize(fixture, destination)
    assert stat_mode(destination) == 0o700
    assert validation["valid_for_collection"] is True
    assert validation["valid_for_metric"] is False
    assert validation["valid_for_comparison"] is False
    assert set(validation["failure_codes"]) == {
        "CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"}
    assert validation["p0a_evidence"]["failure_codes"] == ["P0A_ARTIFACT"]
    manifest = json.loads((destination / "manifest.json").read_bytes())
    assert manifest["run_status"] == "completed"
    assert manifest["dds_comm_trace"]["status"] == "exact_correlated"
    assert manifest["dds_comm_trace"]["record_count"] == 3000
    assert manifest["dds_comm_trace"]["exact_record_count"] == 1000
    provenance = json.loads((destination / "environment/ohos_remote_import.json").read_bytes())
    assert provenance["campaign_id"] == "campaign-1"
    assert provenance["boot_id"] == "boot-1"
    assert provenance["device_finalization"]["ready_images_sha256"] == \
        provenance["device_finalization"]["completion_images_sha256"]
    raw_names = {item.name for item in (destination / "raw/comm_trace").iterdir()}
    assert "directory_finalization.json" in raw_names
    assert "device_completion.json" not in raw_names
    assert "device_runtime_attestation.json" not in raw_names
    for relative in ("normalized/identity_trace.jsonl", "capability/hook_callgraph.json",
                     "normalized/comm_events.jsonl", "normalized/latency_samples.jsonl",
                     "capability/communication_latency.json",
                     "validation/communication_delivery.json", "validation/trace_quality.json",
                     "registration/contract.json", "capability/p0a_evidence_manifest.json"):
        assert (destination / relative).is_file(), relative


def stat_mode(path):
    return path.stat().st_mode & 0o777


def test_cli_and_destination_collision_are_no_replace(tmp_path):
    fixture = _fixture(tmp_path / "source")
    archive, result, request, result_raw, request_raw = fixture
    result_path, request_path = tmp_path / "result.json", tmp_path / "request.json"
    result_path.write_bytes(result_raw); request_path.write_bytes(request_raw)
    destination = tmp_path / "candidate"
    assert main(["materialize-ohos-p0a-candidate", "--archive", str(archive),
                 "--result", str(result_path), "--request", str(request_path),
                 "--destination", str(destination)]) == 0
    sentinel = (destination / "manifest.json").read_bytes()
    assert main(["materialize-ohos-p0a-candidate", "--archive", str(archive),
                 "--result", str(result_path), "--request", str(request_path),
                 "--destination", str(destination)]) == core.EXIT_INVALID
    assert (destination / "manifest.json").read_bytes() == sentinel


def test_rejects_tamper_and_replay_without_output(tmp_path):
    fixture = _fixture(tmp_path / "source")
    archive, result, request, result_raw, request_raw = fixture
    tampered = copy.deepcopy(result); tampered["archive_sha256"] = "0" * 64
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_remote_p0a.materialize_ohos_p0a_candidate(
            archive, tampered, request, tmp_path / "tampered",
            result_sha256=hashlib.sha256(result_raw).hexdigest(),
            request_sha256=hashlib.sha256(request_raw).hexdigest())
    replay = copy.deepcopy(request); replay["campaign_id"] = "other-campaign"
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_remote_p0a.materialize_ohos_p0a_candidate(
            archive, result, replay, tmp_path / "replay",
            result_sha256=hashlib.sha256(result_raw).hexdigest(),
            request_sha256=hashlib.sha256(request_raw).hexdigest())
    assert not (tmp_path / "tampered").exists()
    assert not (tmp_path / "replay").exists()
    assert not list(tmp_path.glob(".mw-perf-ohos-p0a-*"))


def test_partial_failure_cleans_staging(tmp_path):
    fixture = _fixture(tmp_path / "source")
    with patch.object(ohos_remote_p0a.dds_trace, "derive_hook_callgraph",
                      side_effect=ValueError("injected publication failure")):
        with pytest.raises(ValueError, match="injected publication failure"):
            _materialize(fixture, tmp_path / "candidate")
    assert not (tmp_path / "candidate").exists()
    assert not list(tmp_path.glob(".mw-perf-ohos-p0a-*"))


def test_offline_regeneration_is_byte_identical(tmp_path):
    fixture = _fixture(tmp_path / "source")
    first, second = tmp_path / "first", tmp_path / "second"
    _materialize(fixture, first); _materialize(fixture, second)
    for relative in ("manifest.json", "validation.json", "capability/resolution.json",
                     "normalized/identity_trace.jsonl", "capability/hook_callgraph.json",
                     "normalized/comm_events.jsonl", "normalized/latency_samples.jsonl",
                     "capability/communication_latency.json",
                     "validation/communication_delivery.json", "validation/trace_quality.json",
                     "environment/ohos_remote_import.json"):
        assert (first / relative).read_bytes() == (second / relative).read_bytes(), relative


def test_resource_integrity_failure_precedes_output(tmp_path):
    fixture = _fixture(tmp_path / "source")
    destination = tmp_path / "candidate"
    with patch.object(core, "_resource_integrity_errors",
                      return_value=["RESOURCE_TAMPERED:test"]):
        with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError,
                           match="resource"):
            _materialize(fixture, destination)
    assert not destination.exists()


def test_rejects_relative_destination_and_symlink_parent(tmp_path, monkeypatch):
    fixture = _fixture(tmp_path / "source")
    monkeypatch.chdir(tmp_path)
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError, match="absolute"):
        _materialize(fixture, Path("relative-candidate"))
    real_parent = tmp_path / "real-parent"
    real_parent.mkdir()
    linked_parent = tmp_path / "linked-parent"
    linked_parent.symlink_to(real_parent, target_is_directory=True)
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError, match="symlink"):
        _materialize(fixture, linked_parent / "candidate")
    assert not (real_parent / "candidate").exists()
