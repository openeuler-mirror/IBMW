import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import stat
import struct
import sys
import tarfile
from unittest.mock import patch

import pytest

from mw_perf_collect import (core, formal_anchor, ohos_capture, ohos_registration,
                             ohos_remote_p0a)
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
            {"role": "fastdds", "name": "fastdds", "source": "maps",
             "path": "/system/lib64/libfastdds.so", "sha256": "3" * 64},
            {"role": "fastcdr", "name": "fastcdr", "source": "maps",
             "path": "/system/lib64/libfastcdr.so", "sha256": "4" * 64},
            {"role": "cpp_runtime", "name": "libcxx", "source": "maps",
             "path": "/system/lib64/libc++.so", "sha256": "5" * 64},
        ],
        "ready_timeout_ms": 1000, "completion_timeout_ms": 1000,
        "argv": ["/system/bin/candidate"], "capability_id": capability, "env": [],
        "exact_required": True, "path_id": "path-1", "run_id": "run-1",
        "run_root": "/data/capture", "timeout_ms": 5000,
        "topology_generation": "gen-1", "topology_sha256": topology_sha256,
        "trace_mode": "full", "request_challenge": challenge,
    }
    registration = {
        "schema": ohos_registration.SCHEMA, "registration_role": "production_anchor",
        "registration_id": "registration-1",
        "capability_id": capability, "campaign_id": "campaign-1",
        "device": {"serial": "device-1", "os_name": "OpenHarmony", "os_version": "5.0",
            "build_id": "build-1", "abi": "arm64-v8a", "os_image_sha256": "6" * 64},
        "exact_boot_id": "boot-1",
        "toolchain": {"ohos_sdk_id": "sdk-1", "ohos_sdk_sha256": "7" * 64,
            "compiler_id": "clang-1", "compiler_sha256": "8" * 64},
        "source": {"commit": "9" * 40, "tree_sha256": "a" * 64},
        "capture_agent": {"path": "/system/bin/mw_perf_ohos_capture_agent",
                          "sha256": "f" * 64},
        "runtime_images": request["runtime_images"],
        "build_identity": {"formula": ohos_registration.BUILD_FORMULA, "sha256": "0" * 64},
        "threat_model": ohos_registration.THREAT_MODEL,
        "trust_classification": ohos_registration.TRUST_CLASSIFICATION,
    }
    approval = {
        "schema": ohos_registration.APPROVAL_SCHEMA,
        "approval_identity": "operator@example",
        "approved_at_utc": "2026-09-04T00:00:00Z",
        "campaign_id": registration["campaign_id"],
        "capability_id": registration["capability_id"],
        "device_serial": registration["device"]["serial"],
        "exact_boot_id": registration["exact_boot_id"],
        "preregistration_declaration_sha256":
            ohos_registration.preregistration_declaration_sha256(registration),
        "decision": "approved", "threat_model": ohos_registration.THREAT_MODEL,
    }
    approval_raw = ohos_registration.canonical_bytes(approval)
    approval_path = tmp_path / "operator-approval.json"
    approval_path.write_bytes(approval_raw); approval_path.chmod(0o600)
    approval_sha256 = hashlib.sha256(approval_raw).hexdigest()
    registration["approval_artifact"] = {
        "schema": ohos_registration.APPROVAL_SCHEMA, "sha256": approval_sha256}
    registration["build_identity"]["sha256"] = ohos_registration.build_identity_sha256(registration)
    registration_raw = ohos_registration.canonical_bytes(registration)
    registration_path = tmp_path / "registration.json"
    registration_path.write_bytes(registration_raw)
    registration_path.chmod(0o600)
    request["trust_registration"] = {"id": registration["registration_id"],
        "sha256": hashlib.sha256(registration_raw).hexdigest()}
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
        "trust_registration": request["trust_registration"],
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
              "child_start_time_ticks": ticks, "reasons": [],
              "trust_registration": request["trust_registration"]}
    result_raw, request_raw = _json(result) + b"\n", _json(request) + b"\n"
    return (archive, result, request, result_raw, request_raw, registration_path,
            hashlib.sha256(registration_raw).hexdigest(), approval_path, approval_sha256)


def _materialize(fixture, destination):
    (archive, result, request, result_raw, request_raw, registration,
     registration_sha256, approval, approval_sha256) = fixture
    return ohos_remote_p0a.materialize_ohos_p0a_candidate(
        archive, result, request, destination,
        result_raw=result_raw, request_raw=request_raw,
        registration_path=registration, registration_sha256=registration_sha256,
        approval_path=approval, approval_sha256=approval_sha256)


def test_materializes_complete_candidate_from_native_trace(tmp_path):
    fixture = _fixture(tmp_path / "source")
    destination = tmp_path / "candidate"
    validation = _materialize(fixture, destination)
    assert stat_mode(destination) == 0o700
    assert validation["valid_for_collection"] is True
    assert validation["valid_for_metric"] is False
    assert validation["valid_for_comparison"] is False
    assert set(validation["failure_codes"]) == {"CANDIDATE_ONLY", "METRIC_AVAILABLE"}
    assert validation["p0a_evidence"]["valid"] is True
    assert validation["p0a_evidence"]["failure_codes"] == []
    manifest = json.loads((destination / "manifest.json").read_bytes())
    assert manifest["run_status"] == "completed"
    assert manifest["dds_comm_trace"]["status"] == "exact_correlated"
    assert manifest["dds_comm_trace"]["record_count"] == 3000
    assert manifest["dds_comm_trace"]["exact_record_count"] == 1000
    assert manifest["artifact_kind"] == "ohos_remote_p0a_candidate"
    assert "start_time" not in manifest and "end_time" not in manifest
    assert manifest["remote_timing"] == {
        "schema": "mw_perf.ohos_remote_timing.v1",
        "start_time": "unavailable", "end_time": "unavailable",
        "reason": "ohos_remote_capture_v2_did_not_export_wall_clock_boundaries",
    }
    assert manifest["candidate_only"] is True
    assert manifest["formal_p0a"] is False
    assert manifest["metric_availability"] == "unsupported"
    assert manifest["valid_for_metric"] is False
    assert manifest["valid_for_comparison"] is False
    assert manifest["sample_count"] == 0
    assert manifest["sample_artifacts"]["collection"] == "not_collected"
    assert not (destination / "raw/process.jsonl").exists()
    assert not (destination / "raw/network.jsonl").exists()
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
                     "registration/contract.json", "capability/p0a_evidence_manifest.json",
                     "registration/resolution_declarations.json",
                     "registration/ohos_capture_registration.json",
                     "registration/operator_approval.json",
                     "validation/remote_capture_envelope.json",
                     "raw/remote_capture/request.json", "raw/remote_capture/result.json",
                     "raw/remote_capture/archive.tar"):
        assert (destination / relative).is_file(), relative
    (archive, _result, _request, result_raw, request_raw, _registration, _sha,
     approval, _) = fixture
    assert (destination / "raw/remote_capture/request.json").read_bytes() == request_raw
    assert (destination / "raw/remote_capture/result.json").read_bytes() == result_raw
    assert (destination / "raw/remote_capture/archive.tar").read_bytes() == archive.read_bytes()
    assert (destination / "registration/operator_approval.json").read_bytes() == approval.read_bytes()
    resolution = json.loads((destination / "capability/resolution.json").read_bytes())
    assert [(item["role"], item["source"]) for item in resolution["declarations"]] == \
        list(ohos_registration.RUNTIME_LAYOUT)
    assert len(resolution["ready_observations"]) == 5
    quality = json.loads((destination / "validation/trace_quality.json").read_bytes())
    assert quality["support"] == {"level": "remote_candidate_run_only",
        "formal_p0a_completion": False, "failure_code": "P0A_ARTIFACT"}
    envelope = json.loads((destination / "validation/remote_capture_envelope.json").read_bytes())
    assert envelope["trust_classification"] == ohos_registration.TRUST_CLASSIFICATION
    forbidden = {"size", "start_address", "end_address", "permissions", "timestamp",
                 "cpu_seconds", "rss_bytes", "pss_bytes", "threads"}
    assert not forbidden.intersection({key for observation in
        resolution["ready_observations"] + resolution["completion_observations"]
        for key in observation})


def stat_mode(path):
    return path.stat().st_mode & 0o777


def test_cli_and_destination_collision_are_no_replace(tmp_path):
    fixture = _fixture(tmp_path / "source")
    (archive, result, request, result_raw, request_raw, registration,
     registration_sha, approval, approval_sha) = fixture
    result_path, request_path = tmp_path / "result.json", tmp_path / "request.json"
    result_path.write_bytes(result_raw); request_path.write_bytes(request_raw)
    destination = tmp_path / "candidate"
    assert main(["materialize-ohos-p0a-candidate", "--archive", str(archive),
                 "--result", str(result_path), "--request", str(request_path),
                 "--registration", str(registration),
                 "--registration-sha256", registration_sha,
                 "--approval", str(approval), "--approval-sha256", approval_sha,
                 "--destination", str(destination)]) == 0
    sentinel = (destination / "manifest.json").read_bytes()
    assert main(["materialize-ohos-p0a-candidate", "--archive", str(archive),
                 "--result", str(result_path), "--request", str(request_path),
                 "--registration", str(registration),
                 "--registration-sha256", registration_sha,
                 "--approval", str(approval), "--approval-sha256", approval_sha,
                 "--destination", str(destination)]) == core.EXIT_INVALID
    assert (destination / "manifest.json").read_bytes() == sentinel


def test_rejects_tamper_and_replay_without_output(tmp_path):
    fixture = _fixture(tmp_path / "source")
    (archive, result, request, result_raw, request_raw, registration,
     registration_sha, approval, approval_sha) = fixture
    tampered = copy.deepcopy(result); tampered["archive_sha256"] = "0" * 64
    with pytest.raises((ohos_capture.CaptureSemanticError,
                        ohos_remote_p0a.OhosP0aMaterializationError)):
        ohos_remote_p0a.materialize_ohos_p0a_candidate(
            archive, tampered, request, tmp_path / "tampered",
            result_raw=result_raw, request_raw=request_raw,
            registration_path=registration, registration_sha256=registration_sha,
            approval_path=approval, approval_sha256=approval_sha)
    replay = copy.deepcopy(request); replay["campaign_id"] = "other-campaign"
    with pytest.raises((ohos_capture.CaptureSemanticError,
                        ohos_remote_p0a.OhosP0aMaterializationError)):
        ohos_remote_p0a.materialize_ohos_p0a_candidate(
            archive, result, replay, tmp_path / "replay",
            result_raw=result_raw, request_raw=request_raw,
            registration_path=registration, registration_sha256=registration_sha,
            approval_path=approval, approval_sha256=approval_sha)
    assert not (tmp_path / "tampered").exists()
    assert not (tmp_path / "replay").exists()
    assert not list(tmp_path.glob(".mw-perf-ohos-p0a-*"))


def test_registration_cli_create_and_validate(tmp_path):
    fixture = _fixture(tmp_path / "source")
    registration_path = fixture[5]
    approval_path, approval_sha = fixture[7], fixture[8]
    draft = json.loads(registration_path.read_bytes())
    draft.pop("build_identity")
    draft.pop("approval_artifact")
    draft_path = tmp_path / "registration-input.json"
    draft_path.write_bytes(_json(draft))
    output = tmp_path / "registration-created.json"
    assert main(["create-ohos-p0a-registration", "--input", str(draft_path),
                 "--approval", str(approval_path), "--approval-sha256", approval_sha,
                 "--output", str(output)]) == 0
    created = json.loads(output.read_bytes())
    assert created["approval_artifact"]["sha256"] == approval_sha
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    assert main(["validate-ohos-p0a-registration", "--registration", str(output),
                 "--sha256", digest, "--approval", str(approval_path),
                 "--approval-sha256", approval_sha]) == 0
    assert main(["validate-ohos-p0a-registration", "--registration", str(output),
                 "--sha256", "0" * 64, "--approval", str(approval_path),
                 "--approval-sha256", approval_sha]) == core.EXIT_INVALID
    assert main(["validate-ohos-p0a-registration", "--registration", str(output),
                 "--sha256", digest, "--approval", str(approval_path),
                 "--approval-sha256", created["build_identity"]["sha256"]]) == core.EXIT_INVALID


@pytest.mark.parametrize("field", [
    "campaign", "device_serial", "os_name", "os_version", "os_build", "abi", "os_image",
    "boot", "sdk_id", "sdk_hash", "compiler_id", "compiler_hash", "source_commit",
    "source_tree", "build", "image", "agent", "approval_reference",
])
def test_rejects_each_registration_trust_mismatch(tmp_path, field):
    fixture = list(_fixture(tmp_path / "source"))
    registration_path = fixture[5]
    assert isinstance(registration_path, Path)
    registration = json.loads(registration_path.read_bytes())
    if field == "campaign":
        registration["campaign_id"] = "campaign-other"
    elif field == "device_serial":
        registration["device"]["serial"] = "device-other"
    elif field == "os_name":
        registration["device"]["os_name"] = "OHOS-other"
    elif field == "os_version":
        registration["device"]["os_version"] = "6.0"
    elif field == "os_build":
        registration["device"]["build_id"] = "build-other"
    elif field == "abi":
        registration["device"]["abi"] = "x86_64"
    elif field == "os_image":
        registration["device"]["os_image_sha256"] = "1" * 64
    elif field == "boot":
        registration["exact_boot_id"] = "boot-other"
    elif field == "sdk_id":
        registration["toolchain"]["ohos_sdk_id"] = "sdk-other"
    elif field == "sdk_hash":
        registration["toolchain"]["ohos_sdk_sha256"] = "1" * 64
    elif field == "compiler_id":
        registration["toolchain"]["compiler_id"] = "clang-other"
    elif field == "compiler_hash":
        registration["toolchain"]["compiler_sha256"] = "1" * 64
    elif field == "source_commit":
        registration["source"]["commit"] = "1" * 40
    elif field == "source_tree":
        registration["source"]["tree_sha256"] = "1" * 64
    elif field == "build":
        registration["build_identity"]["sha256"] = "1" * 64
    elif field == "image":
        registration["runtime_images"][2]["sha256"] = "1" * 64
    elif field == "agent":
        registration["capture_agent"]["sha256"] = "1" * 64
    else:
        registration["approval_artifact"]["sha256"] = "1" * 64
    if field in {"sdk_id", "sdk_hash", "compiler_id", "compiler_hash", "source_commit",
                 "source_tree", "image", "agent"}:
        registration["build_identity"]["sha256"] = \
            ohos_registration.build_identity_sha256(registration)
    changed_raw = ohos_registration.canonical_bytes(registration)
    changed_path = tmp_path / f"registration-{field}.json"
    changed_path.write_bytes(changed_raw)
    changed_path.chmod(0o600)
    fixture[5] = changed_path
    fixture[6] = hashlib.sha256(changed_raw).hexdigest()
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError):
        _materialize(tuple(fixture), tmp_path / f"candidate-{field}")


def _any_boot_registration(registration):
    value = copy.deepcopy(registration)
    value.pop("exact_boot_id")
    value["boot_policy"] = {"mode": "any_boot",
        "os_image_sha256": value["device"]["os_image_sha256"]}
    return value


def test_any_boot_is_rejected_by_create_validate_and_materialize(tmp_path):
    fixture = list(_fixture(tmp_path / "source"))
    registration_path = fixture[5]
    assert isinstance(registration_path, Path)
    registration = json.loads(registration_path.read_bytes())
    any_boot = _any_boot_registration(registration)
    with pytest.raises(ohos_registration.OhosRegistrationError):
        ohos_registration.validate_registration(any_boot)

    draft = copy.deepcopy(any_boot)
    draft.pop("build_identity")
    draft.pop("approval_artifact")
    draft_path = tmp_path / "any-boot-draft.json"
    draft_path.write_bytes(_json(draft))
    assert main(["create-ohos-p0a-registration", "--input", str(draft_path),
                 "--approval", str(fixture[7]), "--approval-sha256", fixture[8],
                 "--output", str(tmp_path / "created.json")]) == core.EXIT_INVALID
    assert not (tmp_path / "created.json").exists()

    raw = ohos_registration.canonical_bytes(any_boot)
    path = tmp_path / "any-boot.json"
    path.write_bytes(raw); path.chmod(0o600)
    digest = hashlib.sha256(raw).hexdigest()
    assert main(["validate-ohos-p0a-registration", "--registration", str(path),
                 "--sha256", digest, "--approval", str(fixture[7]),
                 "--approval-sha256", fixture[8]]) == core.EXIT_INVALID
    fixture[5], fixture[6] = path, digest
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError):
        _materialize(tuple(fixture), tmp_path / "candidate")


def test_any_boot_is_rejected_by_offline_revalidation(tmp_path):
    destination = tmp_path / "candidate"
    _materialize(_fixture(tmp_path / "source"), destination)
    path = destination / "registration/ohos_capture_registration.json"
    registration = _any_boot_registration(json.loads(path.read_bytes()))
    raw = ohos_registration.canonical_bytes(registration)
    path.write_bytes(raw)
    manifest_path = destination / "capability/p0a_evidence_manifest.json"
    manifest = json.loads(manifest_path.read_bytes())
    entry = next(item for item in manifest["entries"]
                 if item["relative_path"] == "registration/ohos_capture_registration.json")
    entry["sha256"] = hashlib.sha256(raw).hexdigest()
    manifest["required"] = [item for item in manifest["entries"] if item["status"] == "required"]
    manifest_path.write_bytes(_json(manifest) + b"\n")
    validation = core.validate_dir(destination, profile="p0a")
    assert validation["p0a_evidence"]["valid"] is False
    assert "P0A_REMOTE_CAPTURE" in validation["p0a_evidence"]["failure_codes"]


def test_remote_resolution_schemas_require_five_ordered_images(tmp_path):
    destination = tmp_path / "candidate"
    _materialize(_fixture(tmp_path / "source"), destination)
    schemas = Path(core.__file__).parent / "schemas"
    for relative, schema_name, fields in (
            ("capability/resolution.json", "p0a-ohos-remote-resolution.schema.v1.json",
             ("declarations", "ready_observations", "completion_observations", "matches")),
            ("registration/resolution_declarations.json",
             "p0a-ohos-remote-resolution-declarations.schema.v1.json", ("declarations",))):
        value = json.loads((destination / relative).read_bytes())
        schema = json.loads((schemas / schema_name).read_bytes())
        assert core._schema_errors(value, schema) == []
        for field in fields:
            reordered = copy.deepcopy(value)
            reordered[field][0], reordered[field][1] = reordered[field][1], reordered[field][0]
            assert core._schema_errors(reordered, schema), field
            shortened = copy.deepcopy(value); shortened[field].pop()
            assert core._schema_errors(shortened, schema), field


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


@pytest.mark.parametrize("relative", [
    "raw/remote_capture/request.json", "raw/remote_capture/result.json",
    "raw/remote_capture/archive.tar", "validation/remote_capture_envelope.json",
    "capability/resolution.json", "registration/resolution_declarations.json",
    "registration/ohos_capture_registration.json", "registration/operator_approval.json",
])
def test_each_remote_binding_tamper_invalidates_candidate(tmp_path, relative):
    destination = tmp_path / "candidate"
    _materialize(_fixture(tmp_path / "source"), destination)
    before = core.validate_dir(destination, profile="p0a")
    target = destination / relative
    raw = target.read_bytes()
    if relative.endswith("archive.tar"):
        changed = raw[:-1] + bytes([raw[-1] ^ 1])
    elif relative.endswith("request.json") or relative.endswith("result.json"):
        changed = raw + b" "
    else:
        value = json.loads(raw)
        if relative.endswith("capability/resolution.json"):
            value["ready_observations"][0]["inode"] += 1
        elif relative.endswith("resolution_declarations.json"):
            value["declarations"][0]["sha256"] = "0" * 64
        elif relative.endswith("ohos_capture_registration.json"):
            value["approval_artifact"]["sha256"] = "0" * 64
        elif relative.endswith("operator_approval.json"):
            value["approval_identity"] = "tampered@example"
        else:
            value["archive"]["sha256"] = "0" * 64
        changed = _json(value) + b"\n"
    target.write_bytes(changed)
    after = core.validate_dir(destination, profile="p0a")
    assert before["p0a_evidence"]["valid"] is True
    assert before["p0a_evidence"]["failure_codes"] == []
    assert after != before
    assert after["p0a_evidence"]["valid"] is False
    assert "P0A_REMOTE_CAPTURE" in after["p0a_evidence"]["failure_codes"]


def test_standard_profile_rejects_corrupt_archive_and_manifest_promotion(tmp_path):
    destination = tmp_path / "candidate"
    _materialize(_fixture(tmp_path / "source"), destination)
    clean = core.validate_dir(destination, profile="standard")
    assert clean["valid_for_collection"] is True
    assert clean["valid_for_metric"] is False
    assert clean["valid_for_comparison"] is False
    assert clean["p0a_evidence"]["valid"] is True
    assert clean["p0a_evidence"]["failure_codes"] == []

    archive = destination / "raw/remote_capture/archive.tar"
    raw = archive.read_bytes()
    archive.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
    corrupt = core.validate_dir(destination, profile="standard")
    assert corrupt["valid_for_collection"] is False
    assert "P0A_REMOTE_CAPTURE" in corrupt["p0a_evidence"]["failure_codes"]

    archive.write_bytes(raw)
    manifest_path = destination / "manifest.json"
    manifest = json.loads(manifest_path.read_bytes())
    manifest.update({"candidate_only": False, "metric_availability": "supported",
                     "valid_for_metric": True, "valid_for_comparison": True,
                     "validation_profile": "standard", "p0a_evidence_required": False,
                     "start_time": 1, "end_time": 2})
    manifest.pop("artifact_kind")
    manifest.pop("remote_timing")
    manifest["dds_comm_trace"]["registration_contract_version"] = \
        "mw_perf.p0a_contract.v8"
    manifest_path.write_bytes(_json(manifest) + b"\n")
    promoted = core.validate_dir(destination, profile="standard")
    assert promoted["valid_for_collection"] is False
    assert promoted["valid_for_metric"] is False
    assert promoted["valid_for_comparison"] is False
    assert any(rule["name"] == "remote_invariants" and not rule["passed"]
               for rule in promoted["rules"])


@pytest.mark.parametrize("profile", ["standard", "p0a"])
def test_formal_p0a_promotion_exploit_invalidates_collection_and_evidence(tmp_path, profile):
    destination = tmp_path / "candidate"
    _materialize(_fixture(tmp_path / "source"), destination)
    manifest_path = destination / "manifest.json"
    manifest = json.loads(manifest_path.read_bytes())
    manifest["formal_p0a"] = True
    manifest_path.write_bytes(_json(manifest) + b"\n")
    validation = core.validate_dir(destination, profile=profile)
    assert validation["valid_for_collection"] is False
    assert validation["p0a_evidence"]["valid"] is False
    assert "P0A_REMOTE_CAPTURE" in validation["p0a_evidence"]["failure_codes"]
    assert any(rule["name"] == "remote_invariants" and not rule["passed"]
               for rule in validation["rules"])


@pytest.mark.parametrize("field,value", [
    ("campaign_id", "campaign-other"), ("device_serial", "device-other"),
    ("exact_boot_id", "boot-other"), ("capability_id", "other-capability"),
])
def test_approval_replay_is_rejected(tmp_path, field, value):
    fixture = _fixture(tmp_path / "source")
    registration = json.loads(fixture[5].read_bytes())
    approval = json.loads(fixture[7].read_bytes())
    approval[field] = value
    raw = ohos_registration.canonical_bytes(approval)
    with pytest.raises(ohos_registration.OhosRegistrationError):
        ohos_registration.validate_registration(
            registration, raw=fixture[5].read_bytes(), expected_sha256=fixture[6],
            approval=approval, approval_raw=raw,
            expected_approval_sha256=hashlib.sha256(raw).hexdigest())


@pytest.mark.parametrize("field,value", [
    ("schema", "other.schema"), ("approval_identity", ""),
    ("approved_at_utc", "2026-09-04T01:00:00+01:00"),
    ("campaign_id", "campaign-other"), ("capability_id", "other-capability"),
    ("device_serial", "device-other"), ("exact_boot_id", "boot-other"),
    ("preregistration_declaration_sha256", "0" * 64),
    ("decision", "rejected"), ("threat_model", "other-threat-model"),
])
def test_each_operator_approval_field_is_closed_and_bound(tmp_path, field, value):
    fixture = _fixture(tmp_path / "source")
    registration = json.loads(fixture[5].read_bytes())
    approval = json.loads(fixture[7].read_bytes())
    approval[field] = value
    raw = ohos_registration.canonical_bytes(approval)
    with pytest.raises(ohos_registration.OhosRegistrationError):
        ohos_registration.validate_registration(
            registration, raw=fixture[5].read_bytes(), expected_sha256=fixture[6],
            approval=approval, approval_raw=raw,
            expected_approval_sha256=hashlib.sha256(raw).hexdigest())


def test_materializer_requires_external_approval_file_and_expected_hash(tmp_path):
    fixture = list(_fixture(tmp_path / "source"))
    fixture[7] = tmp_path / "missing-approval.json"
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError):
        _materialize(tuple(fixture), tmp_path / "missing")
    fixture = list(_fixture(tmp_path / "other-source"))
    fixture[8] = fixture[6]
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError):
        _materialize(tuple(fixture), tmp_path / "internal-hash")


def test_staging_path_substitution_is_refused_without_deleting_replacement(
        tmp_path, monkeypatch):
    fixture = _fixture(tmp_path / "source")
    real_build = ohos_remote_p0a._build_tree

    def substitute(staging, *args, **kwargs):
        result = real_build(staging, *args, **kwargs)
        named_staging = Path(os.readlink(staging))
        named_staging.rename(named_staging.with_name(named_staging.name + "-original"))
        named_staging.mkdir(mode=0o700)
        (named_staging / "do-not-delete").write_text("replacement")
        return result

    monkeypatch.setattr(ohos_remote_p0a, "_build_tree", substitute)
    with pytest.raises(ohos_remote_p0a.OhosP0aMaterializationError,
                       match="identity changed"):
        _materialize(fixture, tmp_path / "candidate")
    assert not (tmp_path / "candidate").exists()
    replacements = list(tmp_path.glob(".mw-perf-ohos-p0a-*/do-not-delete"))
    assert len(replacements) == 1 and replacements[0].read_text() == "replacement"


def test_publication_fsyncs_staging_before_rename(tmp_path, monkeypatch):
    fixture = _fixture(tmp_path / "source")
    synced = []
    real_fsync = ohos_remote_p0a.os.fsync
    real_publish = ohos_remote_p0a._publish_noreplace

    def recording_fsync(fd):
        info = os.fstat(fd)
        synced.append((info.st_dev, info.st_ino, info.st_mode & 0o777))
        return real_fsync(fd)

    def checked_publish(parent_fd, temporary_name, destination_name):
        staging = os.stat(temporary_name, dir_fd=parent_fd, follow_symlinks=False)
        assert (staging.st_dev, staging.st_ino, 0o700) in synced
        return real_publish(parent_fd, temporary_name, destination_name)

    monkeypatch.setattr(ohos_remote_p0a.os, "fsync", recording_fsync)
    monkeypatch.setattr(ohos_remote_p0a, "_publish_noreplace", checked_publish)
    _materialize(fixture, tmp_path / "candidate")


def test_authority_files_are_synced_and_sync_failure_cleans_staging(
        tmp_path, monkeypatch):
    fixture = _fixture(tmp_path / "source")
    synced_files = set()
    real_fsync = ohos_remote_p0a.os.fsync

    def recording_fsync(fd):
        info = os.fstat(fd)
        if stat.S_ISREG(info.st_mode):
            synced_files.add(Path(os.readlink(f"/proc/self/fd/{fd}")).name)
        return real_fsync(fd)

    monkeypatch.setattr(ohos_remote_p0a.os, "fsync", recording_fsync)
    _materialize(fixture, tmp_path / "synced")
    assert {"manifest.json", "validation.json", "p0a_evidence_manifest.json",
            "resolution.json", "remote_capture_envelope.json"}.issubset(synced_files)

    failed = tmp_path / "failed"
    def failing_fsync(fd):
        if (stat.S_ISREG(os.fstat(fd).st_mode)
                and os.readlink(f"/proc/self/fd/{fd}").endswith("/manifest.json")):
            raise OSError("injected file sync failure")
        return real_fsync(fd)

    monkeypatch.setattr(ohos_remote_p0a.os, "fsync", failing_fsync)
    with pytest.raises(OSError, match="injected file sync failure"):
        _materialize(fixture, failed)
    assert not failed.exists()
    assert not list(tmp_path.glob(".mw-perf-ohos-p0a-*"))


def test_remote_contract_keeps_host_v8_const_and_is_not_formalizable(tmp_path):
    host, host_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    remote, remote_raw = core._trusted_p0a_contract(
        "mw_perf.p0a_ohos_remote_contract.v1")
    version = host["artifact_schemas"]["mw_perf.trace_quality.v1"]["properties"][
        "registration_contract"]["properties"]["version"]["const"]
    assert version == "mw_perf.p0a_contract.v8"
    assert remote["contract_family"] == "ohos_remote_capture_v2"
    assert remote_raw != host_raw
    descriptor = {"campaign_id": "campaign", "role": "exact_anchor",
        "companion_id": "remote", "object_sha256": "0" * 64,
        "bundle_manifest_sha256": "0" * 64, "archive_tree_sha256": "0" * 64,
        "run_id": "run", "evidence_contract": {
            "id": "mw_perf.p0a_ohos_remote_contract.v1",
            "sha256": hashlib.sha256(remote_raw).hexdigest()},
        "root_manifest_sha256": "0" * 64, "evidence_manifest_sha256": "0" * 64,
        "build_identity_sha256": "0" * 64}
    with pytest.raises(ValueError, match="v8 exact anchor"):
        formal_anchor.materialize_formal_anchor(stage1_descriptor=descriptor,
            stage1_object_path=tmp_path / "unused", companion_descriptors={},
            object_store=tmp_path, destination=tmp_path / "formal",
            campaign_id="campaign", companion_id="formal")
