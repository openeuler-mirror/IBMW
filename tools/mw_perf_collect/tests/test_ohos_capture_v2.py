import copy
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
from unittest.mock import patch

import pytest

from mw_perf_collect import core, ohos_capture


H = "a" * 64


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")


def _request():
    return {
        "protocol": "mw_perf.ohos_remote_capture.request.v2", "capture_role": "exact_anchor",
        "campaign_id": "campaign-1", "fault": "none",
        "workload_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "registration_contract": {"id": "mw_perf.p0a_contract.v8",
                                  "sha256": core._RESOURCE_SHA256["contracts/p0a-evidence-contract.v8.json"]},
        "trust_registration": {"id": "registration-1", "sha256": "9" * 64},
        "runtime_images": [
            {"role": "sut", "name": "candidate", "source": "exe", "path": "/system/bin/candidate", "sha256": H},
            {"role": "extension", "name": "dds-ext", "source": "maps", "path": "/system/lib64/libdds_ext.so", "sha256": "b" * 64},
            {"role": "fastdds", "name": "fastdds", "source": "maps", "path": "/system/lib64/libfastdds.so", "sha256": "3" * 64},
            {"role": "fastcdr", "name": "fastcdr", "source": "maps", "path": "/system/lib64/libfastcdr.so", "sha256": "4" * 64},
            {"role": "cpp_runtime", "name": "libcxx", "source": "maps", "path": "/system/lib64/libc++.so", "sha256": "5" * 64},
        ],
        "ready_timeout_ms": 1000, "completion_timeout_ms": 1000,
        "argv": ["/system/bin/candidate"], "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "env": [], "exact_required": True, "path_id": "path-1", "run_id": "run-1",
        "run_root": "/data/capture", "timeout_ms": 5000, "topology_generation": "gen-1",
        "topology_sha256": "c" * 64, "trace_mode": "full", "request_challenge": "d" * 64,
    }


def _fixture(tmp_path, mutate=None):
    tmp_path.mkdir(parents=True, exist_ok=True)
    request = _request()
    identity = {"request_challenge": request["request_challenge"], "campaign_id": "campaign-1",
                "run_id": "run-1", "capture_role": "exact_anchor", "boot_id": "boot-1",
                "child_pid": 42, "child_start_time_ticks": 99}
    completion = identity | {"protocol": "mw_perf.ohos_device_completion.v2",
                             "ready": True, "released": True, "complete": True}
    live = [{"role": image["role"], "name": image["name"], "source": image["source"],
             "expected_path": image["path"], "expected_sha256": image["sha256"],
             "observed_sha256": image["sha256"], "observed_device_major": 8,
             "observed_device_minor": 1, "observed_inode": index + 10,
             "visible": True, "matched": True}
            for index, image in enumerate(request["runtime_images"])]
    phase_hash = hashlib.sha256(_json(live)).hexdigest()
    completion.update({"completion_attested": True, "evidence_released": True,
                       "completion_images": live,
                       "ready_attested": True, "completion_eof_observed": True,
                       "ready_images_sha256": phase_hash,
                       "completion_images_sha256": phase_hash})
    attestation = identity | {"protocol": "mw_perf.ohos_runtime_attestation.v2",
                              "prelaunch_images": request["runtime_images"], "live_images": live,
                              "completion_images": live, "ready_attested": True,
                              "completion_attested": True, "evidence_released": True,
                              "completion_eof_observed": True,
                              "ready_images_sha256": phase_hash,
                              "completion_images_sha256": phase_hash}
    files = {"device_completion.json": _json(completion),
             "device_runtime_attestation.json": _json(attestation), "trace.json": b"{}"}
    source = [{"name": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
              for name, raw in sorted(files.items())]
    tree = hashlib.sha256()
    for item in source:
        tree.update(item["name"].encode() + b"\0" + str(item["size"]).encode() + b"\0"
                    + item["sha256"].encode() + b"\n")
    manifest = {
        "protocol": "mw_perf.ohos_remote_capture.v2", "nonce": "e" * 32,
        "request_challenge": request["request_challenge"], "run_id": "run-1",
        "capability_id": request["capability_id"], "path_id": "path-1",
        "topology_generation": "gen-1", "topology_sha256": request["topology_sha256"],
        "trace_mode": "full", "exact_required": True, "boot_id": "boot-1",
        "agent_executable_sha256": "f" * 64,
        "request_sha256": hashlib.sha256(ohos_capture._canonical_request(request)).hexdigest(),
        "child_pid": 42, "child_start_time_ticks": 99, "wait_status": 0, "graceful": True,
        "no_escalation": True, "descendant_policy": "forbid", "descendant_detected": False,
        "semantic_validation": False, "source_files": source, "archive_tree_sha256": tree.hexdigest(),
        "snapshot_valid": True, "reasons": [], "capture_role": "exact_anchor",
        "campaign_id": "campaign-1", "fault": "none", "workload_id": request["workload_id"],
        "registration_contract": request["registration_contract"], "runtime_images": request["runtime_images"],
        "trust_registration": request["trust_registration"],
        "completion_images": live, "completion_attested": True, "evidence_released": True,
        "ready_attested": True, "completion_eof_observed": True,
        "ready_images_sha256": phase_hash, "completion_images_sha256": phase_hash,
        "ready_observed": True, "release_granted": True, "completion_observed": True,
        "device_completion_sha256": hashlib.sha256(files["device_completion.json"]).hexdigest(),
        "device_runtime_attestation_sha256": hashlib.sha256(files["device_runtime_attestation.json"]).hexdigest(),
    }
    if mutate:
        mutate(request, manifest, files)
    archive = tmp_path / f"capture-{request['request_challenge']}.tar"
    with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, raw in [("device_capture_manifest.json", _json(manifest)), *sorted(files.items())]:
            info = tarfile.TarInfo(name); info.size = len(raw); info.mode = 0o600
            info.uid = info.gid = info.mtime = 0; info.type = tarfile.REGTYPE
            tar.addfile(info, io.BytesIO(raw))
    tar_raw = archive.read_bytes()
    used = ((len(tar_raw.rstrip(b"\0")) + 511) // 512) * 512
    archive.write_bytes(tar_raw[:used] + b"\0" * 1024)
    os.chmod(archive, 0o600)
    raw = archive.read_bytes()
    result = {"protocol": "mw_perf.ohos_remote_capture.result.v2", "snapshot_valid": True,
              "semantic_validation": False, "archive_path": f"/data/capture/{archive.name}",
              "request_challenge": request["request_challenge"], "archive_sha256": hashlib.sha256(raw).hexdigest(),
              "archive_size": len(raw), "child_pid": 42, "child_start_time_ticks": 99,
              "reasons": [], "trust_registration": request["trust_registration"]}
    return archive, result, request


def test_v2_happy_fixture_is_semantic_only(tmp_path):
    archive, result, request = _fixture(tmp_path)
    with patch.object(ohos_capture, "_materialize_and_validate", return_value={"status": "supported"}) as validate:
        imported = ohos_capture.validate_capture_semantics(archive, result, request)
    assert imported["semantic_validation"] is True and imported["formal_p0a"] is False
    assert validate.call_args.kwargs["contract_version"] == "mw_perf.p0a_contract.v8"
    assert "device_completion.json" not in validate.call_args.args[0]


@pytest.mark.parametrize(("field", "value"), [("capture_role", "fault_runtime_matrix"),
                                                ("fault", "rollback"), ("run_root", "/data/../capture")])
def test_v2_rejects_unsupported_roles_faults_and_paths(field, value):
    request = _request(); request[field] = value
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_capture._canonical_request(request)


def test_v2_rejects_runtime_image_semantic_mismatches():
    request = _request()
    request["runtime_images"][0]["path"] = "/system/bin/other"
    with pytest.raises(ohos_capture.CaptureSemanticError, match=r"argv\[0\]"):
        ohos_capture._canonical_request(request)

    request = _request()
    request["runtime_images"][1].update(
        role="sut", name=request["runtime_images"][0]["name"], source="exe")
    with pytest.raises(ohos_capture.CaptureSemanticError, match="must be unique"):
        ohos_capture._canonical_request(request)


@pytest.mark.parametrize("mutate", [
    lambda request: request.update(timeout_ms=86_400_001),
    lambda request: request.update(run_id="bad/run"),
    lambda request: request.update(argv=[""]),
    lambda request: request.update(argv=["/system/bin/candidate", "non-ascii-\u00e9"]),
    lambda request: request.update(env=["EMPTY="]),
    lambda request: request.update(env=["MW_PERF_FORBIDDEN=value"]),
    lambda request: request.update(env=["NAME=one", "NAME=two"]),
    lambda request: request["runtime_images"][1].update(name="bad/name"),
    lambda request: request["runtime_images"][1].update(path="/system/lib64/bad|image.so"),
])
def test_v2_request_domain_matches_native_rejections(mutate):
    request = _request()
    mutate(request)
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_capture._canonical_request(request)


def test_v2_request_accepts_native_timeout_maximum_and_ascii_literals():
    request = _request()
    request["timeout_ms"] = 86_400_000
    request["env"] = ["NAME=value with spaces", "_SECOND=!@#"]
    assert ohos_capture._canonical_request(request)


def test_v2_closed_schemas_and_registry():
    root = Path(ohos_capture.__file__).parent
    for name in ("ohos-capture-request.schema.v2.json", "ohos-capture-result.schema.v2.json",
                 "ohos-capture-manifest.schema.v2.json", "ohos-device-completion.schema.v2.json",
                 "ohos-runtime-attestation.schema.v2.json",
                 "ohos-p0a-operator-approval.schema.v1.json",
                 "ohos-p0a-registration.schema.v1.json"):
        schema = json.loads((root / "schemas" / name).read_bytes())
        assert schema["additionalProperties"] is False
        assert f"schemas/{name}" in core._RESOURCE_SHA256


def test_v2_rejects_replay_and_hash_tamper(tmp_path):
    archive, result, request = _fixture(tmp_path)
    replay = copy.deepcopy(request); replay["campaign_id"] = "other"
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_capture.validate_capture_semantics(archive, result, replay)

    archive, result, request = _fixture(
        tmp_path / "tamper", lambda _r, manifest, _f: manifest.update(device_completion_sha256="0" * 64))
    with pytest.raises(ohos_capture.CaptureSemanticError, match="attestation hash"):
        ohos_capture.validate_capture_semantics(archive, result, request)


def test_v2_rejects_extra_member_and_attestation_schema(tmp_path):
    def extra(_request, _manifest, files):
        files["unexpected.json"] = b"{}"
    archive, result, request = _fixture(tmp_path / "extra", extra)
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_capture.validate_capture_semantics(archive, result, request)

    def malformed(_request, _manifest, files):
        value = json.loads(files["device_completion.json"]); value["extra"] = True
        files["device_completion.json"] = _json(value)
    archive, result, request = _fixture(tmp_path / "schema", malformed)
    with pytest.raises(ohos_capture.CaptureSemanticError):
        ohos_capture.validate_capture_semantics(archive, result, request)
