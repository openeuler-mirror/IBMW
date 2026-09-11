import copy
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tarfile

import pytest

from mw_perf_collect import validate_ohos_remote_fault_companion
from mw_perf_collect import campaign, core, ohos_registration, ohos_remote_fault
from mw_perf_collect.cli import main
from test_ohos_remote_p0a import _fixture, _json


def _tar(path, manifest, files):
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as stream:
        for name, raw in [("device_capture_manifest.json", _json(manifest)), *sorted(files.items())]:
            info = tarfile.TarInfo(name)
            info.size = len(raw); info.mode = 0o600; info.uid = info.gid = info.mtime = 0
            info.type = tarfile.REGTYPE
            stream.addfile(info, io.BytesIO(raw))
    raw = path.read_bytes()
    used = ((len(raw.rstrip(b"\0")) + 511) // 512) * 512
    path.write_bytes(raw[:used] + b"\0" * 1024)
    path.chmod(0o600)


def _tree(files):
    source = [{"name": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
              for name, raw in sorted(files.items())]
    digest = hashlib.sha256()
    for item in source:
        digest.update(item["name"].encode() + b"\0" + str(item["size"]).encode() + b"\0"
                      + item["sha256"].encode() + b"\n")
    return source, digest.hexdigest()


def _mutate_trace(files, case_id, run_id):
    stage_meta_name = next(name for name in files if name.endswith(".meta.json") and ".exact." not in name)
    stage_seal_name = next(name for name in files if name.endswith(".seal.json") and ".exact." not in name)
    exact_meta_name = next(name for name in files if name.endswith(".exact.meta.json"))
    exact_seal_name = next(name for name in files if name.endswith(".exact.seal.json"))
    stage_name = next(name for name in files if name.endswith(".ibct"))
    exact_name = next(name for name in files if name.endswith(".ibce"))
    producer_run = "wrong-runtime-run" if case_id == "producer_identity_mismatch" else run_id
    for name in (stage_meta_name, stage_seal_name, exact_meta_name, exact_seal_name):
        value = json.loads(files[name]); value["run_id"] = producer_run
        if name == stage_seal_name and case_id == "stage_loss": value["dropped"] = 1
        if name == exact_seal_name and case_id == "callback_loss":
            value["dropped"] = 1; value["finalized"] = False
        files[name] = _json(value)
    stage = bytearray(files[stage_name]); exact = bytearray(files[exact_name])
    stage[40:72] = hashlib.sha256(files[stage_meta_name]).digest()
    exact[32:64] = hashlib.sha256(files[exact_meta_name]).digest()
    if case_id == "stage_loss": struct.pack_into("<Q", stage, 112, 1)
    if case_id == "callback_loss": struct.pack_into("<Q", exact, 88, 1)
    if case_id == "stage_status":
        stage[256 + 61] = 1
        stage[136:168] = hashlib.sha256(stage[256:]).digest()
    if case_id == "callback_status":
        exact[160 + 65] = 1
        exact[128:160] = hashlib.sha256(exact[160:]).digest()
    files[stage_name], files[exact_name] = bytes(stage), bytes(exact)


def _fault_fixture(root, case_id, anchor_registration, anchor_raw):
    fixture = list(_fixture(root))
    archive, result, request = fixture[:3]
    entries = __import__("mw_perf_collect.ohos_capture", fromlist=["_parse_ustar"])._parse_ustar(archive.read_bytes())
    manifest, files = json.loads(entries[0][1]), dict(entries[1:])
    index = ohos_remote_fault.CASE_IDS.index(case_id) + 1
    run_id, challenge, nonce = f"fault-run-{index}", f"{index}" * 64, f"{index}" * 32

    registration = json.loads(Path(fixture[5]).read_bytes())
    registration.update({"registration_role": "fault_runtime_subject",
                         "registration_id": f"fault-registration-{index}",
                         "production_anchor": {"registration_id": anchor_registration["registration_id"],
                             "sha256": hashlib.sha256(anchor_raw).hexdigest(),
                             "build_identity_sha256": anchor_registration["build_identity"]["sha256"]}})
    registration["runtime_images"][0]["sha256"] = "1" * 64
    registration["runtime_images"][1]["sha256"] = "6" * 64
    approval = json.loads(Path(fixture[7]).read_bytes())
    approval["preregistration_declaration_sha256"] = \
        ohos_registration.preregistration_declaration_sha256(registration)
    approval_raw = ohos_registration.canonical_bytes(approval)
    registration["approval_artifact"]["sha256"] = hashlib.sha256(approval_raw).hexdigest()
    registration["build_identity"]["sha256"] = ohos_registration.build_identity_sha256(registration)
    registration_raw = ohos_registration.canonical_bytes(registration)
    registration_path, approval_path = root / "fault-registration.json", root / "fault-approval.json"
    registration_path.write_bytes(registration_raw); registration_path.chmod(0o600)
    approval_path.write_bytes(approval_raw); approval_path.chmod(0o600)
    binding = {"id": registration["registration_id"], "sha256": hashlib.sha256(registration_raw).hexdigest()}

    request.update({"capture_role": "fault_runtime_subject", "fault": case_id,
                    "run_id": run_id, "request_challenge": challenge,
                    "trust_registration": binding, "runtime_images": registration["runtime_images"]})
    _mutate_trace(files, case_id, run_id)
    for name in ("device_completion.json", "device_runtime_attestation.json"):
        value = json.loads(files[name])
        value.update({"request_challenge": challenge, "run_id": run_id,
                      "capture_role": "fault_runtime_subject"})
        if name == "device_runtime_attestation.json": value["prelaunch_images"] = request["runtime_images"]
        for image, declared in zip(value.get("live_images", value["completion_images"]), request["runtime_images"]):
            image["expected_sha256"] = image["observed_sha256"] = declared["sha256"]
        if "completion_images" in value:
            for image, declared in zip(value["completion_images"], request["runtime_images"]):
                image["expected_sha256"] = image["observed_sha256"] = declared["sha256"]
        live = value.get("live_images", value["completion_images"])
        image_hash = hashlib.sha256(_json(live)).hexdigest()
        value["ready_images_sha256"] = value["completion_images_sha256"] = image_hash
        files[name] = _json(value)
    completion = json.loads(files["device_completion.json"])
    attestation = json.loads(files["device_runtime_attestation.json"])
    manifest.update({"nonce": nonce, "request_challenge": challenge, "run_id": run_id,
        "capture_role": "fault_runtime_subject", "fault": case_id,
        "trust_registration": binding, "runtime_images": request["runtime_images"],
        "completion_images": attestation["completion_images"],
        "ready_images_sha256": attestation["ready_images_sha256"],
        "completion_images_sha256": attestation["completion_images_sha256"],
        "device_completion_sha256": hashlib.sha256(files["device_completion.json"]).hexdigest(),
        "device_runtime_attestation_sha256": hashlib.sha256(files["device_runtime_attestation.json"]).hexdigest(),
        "request_sha256": hashlib.sha256(_json(request)).hexdigest()})
    manifest["source_files"], manifest["archive_tree_sha256"] = _tree(files)
    archive = root / f"capture-{challenge}.tar"; _tar(archive, manifest, files)
    archive_raw = archive.read_bytes()
    result.update({"archive_path": f"/data/capture/{archive.name}", "request_challenge": challenge,
                   "archive_sha256": hashlib.sha256(archive_raw).hexdigest(),
                   "archive_size": len(archive_raw), "trust_registration": binding})
    return (archive, result, request, _json(result) + b"\n", _json(request) + b"\n",
            registration_path, hashlib.sha256(registration_raw).hexdigest(), approval_path,
            hashlib.sha256(approval_raw).hexdigest())


def _materialize(fixture, destination):
    return ohos_remote_fault.materialize_ohos_remote_fault_subject(
        fixture[0], fixture[1], fixture[2], destination, result_raw=fixture[3],
        request_raw=fixture[4], registration_path=fixture[5], registration_sha256=fixture[6],
        approval_path=fixture[7], approval_sha256=fixture[8])


@pytest.fixture
def matrix_fixture(tmp_path):
    production = _fixture(tmp_path / "production-source")
    anchor = tmp_path / "anchor"; (anchor / "registration").mkdir(parents=True)
    anchor_raw = Path(production[5]).read_bytes()
    (anchor / "registration/ohos_capture_registration.json").write_bytes(anchor_raw)
    (anchor / "registration/operator_approval.json").write_bytes(Path(production[7]).read_bytes())
    anchor_registration = json.loads(anchor_raw)
    subjects = {}
    fixtures = {}
    (tmp_path / "subjects").mkdir()
    for case in ohos_remote_fault.CASE_IDS:
        fixtures[case] = _fault_fixture(tmp_path / f"source-{case}", case, anchor_registration, anchor_raw)
        subjects[case] = tmp_path / "subjects" / case
        _materialize(fixtures[case], subjects[case])
    registry = {
        "schema": ohos_remote_fault.TRUSTED_ROOTS_SCHEMA,
        "campaign_id": anchor_registration["campaign_id"],
        "capability_id": anchor_registration["capability_id"],
        "threat_model": anchor_registration["threat_model"],
        "production": {"registration_sha256": production[6], "approval_sha256": production[8]},
        "cases": [{"case_id": case, "registration_sha256": fixtures[case][6],
                   "approval_sha256": fixtures[case][8]}
                  for case in ohos_remote_fault.CASE_IDS],
    }
    registry_path = tmp_path / "trusted-roots.json"
    registry_raw = ohos_registration.canonical_bytes(registry)
    registry_path.write_bytes(registry_raw); registry_path.chmod(0o600)
    return subjects, fixtures, anchor, registry_path, hashlib.sha256(registry_raw).hexdigest()


def _evaluate(matrix_fixture, destination, anchor=None):
    subjects, _fixtures, production, registry, registry_sha = matrix_fixture
    return ohos_remote_fault.evaluate_ohos_remote_fault_matrix(
        subjects, destination, anchor or production, companion_id="faults",
        trusted_roots_path=registry,
        expected_trusted_roots_sha256=registry_sha)


def _package(matrix_fixture, matrix_root, object_store):
    _subjects, _fixtures, _anchor, registry, registry_sha = matrix_fixture
    return ohos_remote_fault.package_ohos_remote_fault_matrix(
        matrix_root, object_store, campaign_id="campaign-1",
        trusted_roots_path=registry, expected_trusted_roots_sha256=registry_sha)


def test_all_three_apis_and_cli_happy_path(matrix_fixture, tmp_path):
    subjects, fixtures, anchor, registry, registry_sha = matrix_fixture
    matrix_root = tmp_path / "matrix"
    matrix = _evaluate(matrix_fixture, matrix_root)
    assert matrix["companion_id"] == "faults"
    assert [item["case_id"] for item in matrix["cases"]] == list(ohos_remote_fault.CASE_IDS)
    assert all(len(item["runtime_image_hashes"]) == 5 for item in matrix["cases"])
    descriptor = _package(matrix_fixture, matrix_root, tmp_path / "objects")
    object_path = tmp_path / "objects/sha256" / descriptor["object_sha256"][:2] / f"{descriptor['object_sha256']}.tar"
    assert campaign.validate_companion_object(object_path, descriptor)["valid"] is True
    validation = validate_ohos_remote_fault_companion(
        object_path, descriptor, trusted_roots_path=registry,
        expected_trusted_roots_sha256=registry_sha)
    assert validation["valid"] is True
    assert set(validation) == ohos_remote_fault._VALIDATION_KEYS
    assert validation["trusted_roots_sha256"] == registry_sha
    assert validation["production_anchor"] == matrix["production_anchor"]
    assert validation["production_build_identity_sha256"] == \
        matrix["production_build_identity_sha256"]
    assert validation["root_manifest_sha256"] == descriptor["root_manifest_sha256"]
    assert validation["evidence_manifest_sha256"] == descriptor["evidence_manifest_sha256"]
    cli_subject = tmp_path / "cli-subject"
    fixture = fixtures["stage_loss"]
    result_path, request_path = tmp_path / "result.json", tmp_path / "request.json"
    result_path.write_bytes(fixture[3]); request_path.write_bytes(fixture[4])
    assert main(["materialize-ohos-remote-fault", "--archive", str(fixture[0]),
        "--result", str(result_path), "--request", str(request_path), "--registration", str(fixture[5]),
        "--registration-sha256", fixture[6], "--approval", str(fixture[7]),
        "--approval-sha256", fixture[8], "--destination", str(cli_subject)]) == 0
    assert main(["evaluate-ohos-remote-fault", "--subjects", str(tmp_path / "subjects"),
        "--destination", str(tmp_path / "cli-matrix"), "--production-anchor", str(anchor),
        "--companion-id", "cli-faults",
        "--trusted-roots", str(registry), "--trusted-roots-sha256", registry_sha]) == 0
    assert main(["package-ohos-remote-fault", "--matrix", str(matrix_root),
        "--object-store", str(tmp_path / "cli-objects"), "--campaign-id", "campaign-1",
        "--trusted-roots", str(registry),
        "--trusted-roots-sha256", registry_sha]) == 0


@pytest.mark.parametrize("mutation", ["receipt", "archive_registration", "approval_role_anchor",
                                       "offline_request", "v1_request"])
def test_forgery_mix_approval_offline_and_v1_are_rejected(matrix_fixture, tmp_path, mutation):
    subjects, fixtures, anchor, _registry, _registry_sha = matrix_fixture
    subject = subjects["stage_loss"]
    if mutation == "receipt":
        path = subject / "validation/fault_receipt.json"; value = json.loads(path.read_bytes())
        value["subject_run_id"] = "forged"; path.write_bytes(_json(value) + b"\n")
    elif mutation == "archive_registration":
        (subject / "registration/registration.json").write_bytes(
            (subjects["callback_loss"] / "registration/registration.json").read_bytes())
    elif mutation == "approval_role_anchor":
        path = subject / "registration/approval.json"; value = json.loads(path.read_bytes())
        value["preregistration_declaration_sha256"] = "f" * 64; path.write_bytes(_json(value) + b"\n")
    elif mutation == "offline_request":
        path = subject / "raw/request.json"; value = json.loads(path.read_bytes())
        value["run_id"] = "offline-mutated"; path.write_bytes(_json(value) + b"\n")
    else:
        path = subject / "raw/request.json"; value = json.loads(path.read_bytes())
        value["protocol"] = "mw_perf.ohos_remote_capture.request.v1"; path.write_bytes(_json(value) + b"\n")
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError):
        _evaluate(matrix_fixture, tmp_path / "bad")


def test_fault_role_cannot_be_used_as_production(matrix_fixture, tmp_path):
    subjects, _fixtures, _anchor, _registry, _registry_sha = matrix_fixture
    fake = tmp_path / "fake-anchor"; (fake / "registration").mkdir(parents=True)
    (fake / "registration/ohos_capture_registration.json").write_bytes(
        (subjects["stage_loss"] / "registration/registration.json").read_bytes())
    (fake / "registration/operator_approval.json").write_bytes(
        (subjects["stage_loss"] / "registration/approval.json").read_bytes())
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError, match="authority|production_anchor"):
        _evaluate(matrix_fixture, tmp_path / "bad", fake)


@pytest.mark.parametrize("field", ["registration_role", "production_anchor"])
def test_approval_projection_closes_role_and_anchor(matrix_fixture, field):
    subjects, _fixtures, _anchor, _registry, _registry_sha = matrix_fixture
    registration_raw = (subjects["stage_loss"] / "registration/registration.json").read_bytes()
    approval_raw = (subjects["stage_loss"] / "registration/approval.json").read_bytes()
    registration = json.loads(registration_raw)
    if field == "registration_role":
        registration["registration_role"] = "production_anchor"
    else:
        registration["production_anchor"]["sha256"] = "f" * 64
    changed = ohos_registration.canonical_bytes(registration)
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError):
        ohos_remote_fault._authority(
            changed, approval_raw, fault=True,
            expected_registration_sha=hashlib.sha256(changed).hexdigest(),
            expected_approval_sha=hashlib.sha256(approval_raw).hexdigest())


@pytest.mark.parametrize("change", ["missing", "extra", "duplicate", "misordered", "zero"])
def test_matrix_shape_and_zero_identity_regressions(matrix_fixture, tmp_path, change):
    subjects, _fixtures, anchor, _registry, _registry_sha = matrix_fixture
    matrix_root = tmp_path / "matrix"
    _evaluate(matrix_fixture, matrix_root)
    path = matrix_root / "runtime_fault_matrix.json"; value = json.loads(path.read_bytes())
    if change == "missing": value["cases"].pop()
    elif change == "extra": value["cases"].append(copy.deepcopy(value["cases"][0]))
    elif change == "duplicate": value["cases"][1] = copy.deepcopy(value["cases"][0])
    elif change == "misordered": value["cases"][0], value["cases"][1] = value["cases"][1], value["cases"][0]
    else: value["fault_build_identity_sha256"] = "0" * 64
    path.write_bytes(_json(value) + b"\n")
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError):
        _package(matrix_fixture, matrix_root, tmp_path / "objects")


def test_destination_collision_race_is_no_replace_and_partial_write_cleans(matrix_fixture, tmp_path, monkeypatch):
    subjects, _fixtures, anchor, _registry, _registry_sha = matrix_fixture
    destination = tmp_path / "race"
    def collide(kind):
        assert kind == "matrix"; destination.mkdir(); (destination / "sentinel").write_text("keep")
    monkeypatch.setattr(ohos_remote_fault, "_publication_checkpoint", collide)
    with pytest.raises(FileExistsError):
        _evaluate(matrix_fixture, destination)
    assert (destination / "sentinel").read_text() == "keep"
    assert not list(tmp_path.glob(".race-*"))

    monkeypatch.setattr(ohos_remote_fault, "_publication_checkpoint", lambda _kind: None)
    real_write = ohos_remote_fault.os.write
    def fail_write(fd, data):
        if "/.partial-" in os.readlink(f"/proc/self/fd/{fd}"):
            raise OSError("partial write")
        return real_write(fd, data)
    monkeypatch.setattr(ohos_remote_fault.os, "write", fail_write)
    with pytest.raises(OSError, match="partial write"):
        _evaluate(matrix_fixture, tmp_path / "partial")
    assert not (tmp_path / "partial").exists() and not list(tmp_path.glob(".partial-*"))


def test_archive_change_during_read_and_package_source_race(matrix_fixture, tmp_path, monkeypatch):
    subjects, fixtures, anchor, _registry, _registry_sha = matrix_fixture
    fixture = fixtures["stage_loss"]
    real_read = ohos_remote_fault.os.read
    changed = False
    def mutate_read(fd, size):
        nonlocal changed
        data = real_read(fd, size)
        if not changed and Path(os.readlink(f"/proc/self/fd/{fd}")) == fixture[0]:
            changed = True; os.utime(fixture[0], None)
        return data
    monkeypatch.setattr(ohos_remote_fault.os, "read", mutate_read)
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError, match="changed"):
        _materialize(fixture, tmp_path / "changed")
    monkeypatch.setattr(ohos_remote_fault.os, "read", real_read)

    original = fixture[0].read_bytes()
    def mutate_after_validation():
        fixture[0].write_bytes(b"changed after validation")
    monkeypatch.setattr(ohos_remote_fault, "_retention_checkpoint", mutate_after_validation)
    retained = tmp_path / "retained"
    _materialize(fixture, retained)
    assert (retained / "raw/archive.tar").read_bytes() == original
    monkeypatch.setattr(ohos_remote_fault, "_retention_checkpoint", lambda: None)

    matrix_root = tmp_path / "matrix"
    _evaluate(matrix_fixture, matrix_root)
    real_build = campaign.build_companion_object_from_snapshot
    def mutate_then_build(snapshot, store, metadata):
        path = matrix_root / "runtime_fault_matrix.json"
        path.write_bytes(path.read_bytes() + b" ")
        return real_build(snapshot, store, metadata)
    monkeypatch.setattr(campaign, "build_companion_object_from_snapshot", mutate_then_build)
    descriptor = _package(matrix_fixture, matrix_root, tmp_path / "objects")
    object_path = tmp_path / "objects/sha256" / descriptor["object_sha256"][:2] / f"{descriptor['object_sha256']}.tar"
    assert campaign.validate_companion_object(object_path, descriptor)["valid"] is True


def test_valid_trace_and_mislabeled_case_are_rejected(tmp_path):
    production = _fixture(tmp_path / "production")
    anchor_raw = Path(production[5]).read_bytes(); anchor = json.loads(anchor_raw)
    fault = list(_fault_fixture(tmp_path / "fault", "stage_loss", anchor, anchor_raw))
    request = copy.deepcopy(fault[2]); request["fault"] = "callback_loss"
    request_raw = _json(request) + b"\n"
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError):
        ohos_remote_fault.materialize_ohos_remote_fault_subject(
            fault[0], fault[1], request, tmp_path / "mislabeled", result_raw=fault[3],
            request_raw=request_raw, registration_path=fault[5], registration_sha256=fault[6],
            approval_path=fault[7], approval_sha256=fault[8])
    valid = list(_fixture(tmp_path / "valid"))
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError):
        _materialize(valid, tmp_path / "valid-output")


def test_external_registry_root_is_required_and_full_replacement_is_rejected(matrix_fixture, tmp_path):
    subjects, _fixtures, anchor, registry, registry_sha = matrix_fixture
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError, match="external trusted-roots"):
        ohos_remote_fault.evaluate_ohos_remote_fault_matrix(
            subjects, tmp_path / "missing", anchor, companion_id="faults",
            trusted_roots_path=registry,
            expected_trusted_roots_sha256=None)

    replacement = json.loads(registry.read_bytes())
    replacement["production"] = {"registration_sha256": "e" * 64,
                                 "approval_sha256": "d" * 64}
    for index, item in enumerate(replacement["cases"], 1):
        item["registration_sha256"] = f"{index}" * 64
        item["approval_sha256"] = f"{index + 5:x}" * 64
    replacement_raw = ohos_registration.canonical_bytes(replacement)
    replacement_path = tmp_path / "replacement-roots.json"
    replacement_path.write_bytes(replacement_raw); replacement_path.chmod(0o600)
    replacement_sha = hashlib.sha256(replacement_raw).hexdigest()
    assert replacement_sha != registry_sha
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError, match="external trusted-roots"):
        ohos_remote_fault.evaluate_ohos_remote_fault_matrix(
            subjects, tmp_path / "externally-rejected-replacement", anchor,
            companion_id="faults",
            trusted_roots_path=replacement_path,
            expected_trusted_roots_sha256=registry_sha)
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError):
        ohos_remote_fault.evaluate_ohos_remote_fault_matrix(
            subjects, tmp_path / "replacement", anchor,
            companion_id="faults",
            trusted_roots_path=replacement_path,
            expected_trusted_roots_sha256=replacement_sha)


def test_fault_receipt_rejects_additional_or_mismatched_semantic_failure(tmp_path, monkeypatch):
    production = _fixture(tmp_path / "production")
    anchor_raw = Path(production[5]).read_bytes(); anchor = json.loads(anchor_raw)
    fixture = _fault_fixture(tmp_path / "fault", "stage_loss", anchor, anchor_raw)
    monkeypatch.setattr(ohos_remote_fault.ohos_capture, "validate_capture_bytes", lambda *_args: {
        "semantic_validation": False, "fault_error": "imported trace semantic validation failed",
        "formal_p0a": False, "unexpected": True})
    with pytest.raises(ohos_remote_fault.OhosRemoteFaultError, match="exact expected failure"):
        _materialize(fixture, tmp_path / "subject")


@pytest.mark.parametrize("index", range(5))
def test_every_runtime_image_component_rejects_zero_hash(tmp_path, index):
    fixture = _fixture(tmp_path / f"image-{index}")
    registration = json.loads(Path(fixture[5]).read_bytes())
    registration["runtime_images"][index]["sha256"] = "0" * 64
    schema = json.loads((Path(ohos_remote_fault.__file__).parent /
                         "schemas/ohos-p0a-registration.schema.v1.json").read_bytes())
    assert core._schema_errors(registration, schema)


def test_registry_schema_is_closed_and_exactly_five_ordered_cases(matrix_fixture):
    _subjects, _fixtures, _anchor, registry, _registry_sha = matrix_fixture
    schema = json.loads((Path(ohos_remote_fault.__file__).parent /
                         "schemas/ohos-remote-fault-trusted-roots.schema.v1.json").read_bytes())
    value = json.loads(registry.read_bytes())
    for mutation in (
            lambda item: item.update(unexpected=True),
            lambda item: item["cases"].pop(),
            lambda item: item["cases"].reverse()):
        changed = copy.deepcopy(value); mutation(changed)
        assert core._schema_errors(changed, schema)


def test_every_descriptor_authority_rebind_is_rejected(matrix_fixture, tmp_path):
    _subjects, _fixtures, _anchor, registry, registry_sha = matrix_fixture
    matrix_root = tmp_path / "matrix"
    _evaluate(matrix_fixture, matrix_root)
    store = tmp_path / "objects"
    descriptor = _package(matrix_fixture, matrix_root, store)
    object_path = store / "sha256" / descriptor["object_sha256"][:2] / \
        f"{descriptor['object_sha256']}.tar"

    direct_mutations = {
        "campaign_id": lambda item: item.__setitem__("campaign_id", "attacker-campaign"),
        "role": lambda item: item.__setitem__("role", "exact_anchor"),
        "run_id": lambda item: item.__setitem__("run_id", "attacker-run"),
        "companion_id": lambda item: item.__setitem__("companion_id", "attacker-companion"),
        "evidence_contract.id": lambda item: item["evidence_contract"].__setitem__("id", "attacker-contract"),
        "evidence_contract.sha256": lambda item: item["evidence_contract"].__setitem__("sha256", "a" * 64),
        "root_manifest_sha256": lambda item: item.__setitem__("root_manifest_sha256", "a" * 64),
        "evidence_manifest_sha256": lambda item: item.__setitem__("evidence_manifest_sha256", "a" * 64),
        "build_identity_sha256": lambda item: item.__setitem__("build_identity_sha256", "a" * 64),
        "object_sha256": lambda item: item.__setitem__("object_sha256", "a" * 64),
        "bundle_manifest_sha256": lambda item: item.__setitem__("bundle_manifest_sha256", "a" * 64),
        "archive_tree_sha256": lambda item: item.__setitem__("archive_tree_sha256", "a" * 64),
    }
    for field, mutate in direct_mutations.items():
        rebound = copy.deepcopy(descriptor)
        mutate(rebound)
        result = validate_ohos_remote_fault_companion(
            object_path, rebound, trusted_roots_path=registry,
            expected_trusted_roots_sha256=registry_sha)
        assert result["valid"] is False, field

    snapshot = campaign.PayloadSnapshot(campaign._read_canonical_payload_object(object_path))
    metadata_fields = (
        "campaign_id", "role", "companion_id", "run_id", "evidence_contract",
        "root_manifest_sha256", "evidence_manifest_sha256", "build_identity_sha256",
    )
    for field in metadata_fields:
        metadata = {key: copy.deepcopy(descriptor[key]) for key in (
            "campaign_id", "role", "companion_id", "run_id", "evidence_contract",
            "root_manifest_sha256", "evidence_manifest_sha256", "build_identity_sha256")}
        metadata[field] = ({"id": "attacker-contract", "sha256": "a" * 64}
                           if field == "evidence_contract" else
                           "exact_anchor" if field == "role" else
                           "a" * 64 if field.endswith("sha256") else f"attacker-{field}")
        rebound = campaign.build_companion_object_from_snapshot(
            snapshot, tmp_path / f"rebound-{field}", metadata)
        rebound_path = tmp_path / f"rebound-{field}" / "sha256" / \
            rebound["object_sha256"][:2] / f"{rebound['object_sha256']}.tar"
        assert campaign.validate_companion_object(rebound_path, rebound)["valid"] is True
        result = validate_ohos_remote_fault_companion(
            rebound_path, rebound, trusted_roots_path=registry,
            expected_trusted_roots_sha256=registry_sha)
        assert result["valid"] is False, field
        assert result["failure_codes"] == ["OHOS_REMOTE_FAULT_AUTHORITY"]


def test_host_campaign_rejects_remote_contract_family(matrix_fixture, tmp_path):
    matrix_root = tmp_path / "matrix"
    _evaluate(matrix_fixture, matrix_root)
    descriptor = _package(matrix_fixture, matrix_root, tmp_path / "objects")
    object_path = tmp_path / "objects" / "sha256" / descriptor["object_sha256"][:2] / \
        f"{descriptor['object_sha256']}.tar"
    result = campaign._specialized_companion(
        "fault_runtime_matrix", object_path, descriptor)
    assert result["valid"] is False
    assert result["failure_codes"] == ["RUNTIME_FAULT_CONTRACT_FAMILY"]
