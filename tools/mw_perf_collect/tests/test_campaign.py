import hashlib
import io
import json
import os
import stat
import tarfile
from pathlib import Path
from typing import Any

import pytest

import mw_perf_collect.campaign as campaign
from mw_perf_collect.campaign import (
    build_companion_object,
    build_companion_object_from_snapshot,
    capture_payload_snapshot,
    compute_archive_tree,
    finalize_campaign,
    validate_companion_object,
)
from mw_perf_collect.core import _canonical_json_bytes, _schema_errors


H = "a" * 64


def _metadata(**changes):
    value = {
        "campaign_id": "campaign-1",
        "role": "exact_anchor",
        "companion_id": "anchor-1",
        "run_id": "run-1",
        "evidence_contract": {"id": "contract-1", "sha256": H},
        "root_manifest_sha256": "b" * 64,
        "evidence_manifest_sha256": "c" * 64,
        "build_identity_sha256": "d" * 64,
    }
    value.update(changes)
    return value


def _payload(path: Path):
    (path / "nested").mkdir(parents=True)
    (path / "z.txt").write_bytes(b"z\n")
    (path / "nested/a.bin").write_bytes(b"abc")


def _object_path(store: Path, descriptor):
    digest = descriptor["object_sha256"]
    return store / "sha256" / digest[:2] / f"{digest}.tar"


def _store_raw(store: Path, raw: bytes) -> Path:
    digest = hashlib.sha256(raw).hexdigest()
    path = store / "sha256" / digest[:2] / f"{digest}.tar"
    path.parent.mkdir(parents=True)
    path.write_bytes(raw)
    return path


def _tar(members):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, data, kind in members:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o600
            info.uid = info.gid = info.mtime = 0
            info.type = kind
            archive.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
    return output.getvalue()


def _members(path: Path):
    with tarfile.open(path, "r:") as archive:
        result = []
        for item in archive.getmembers():
            stream = archive.extractfile(item)
            assert stream is not None
            result.append((item.name, stream.read(), item.type))
        return result


def test_deterministic_build_validates_and_uses_content_name(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    first = build_companion_object(payload, store, _metadata())
    second = build_companion_object(payload, store, _metadata())
    path = _object_path(store, first)

    assert first == second
    assert path.is_file()
    assert hashlib.sha256(path.read_bytes()).hexdigest() == first["object_sha256"]
    result = validate_companion_object(path, first)
    assert result["valid"]
    assert result["descriptor"] == first
    assert result["bundle_manifest"]["payload_member_count"] == 2
    assert [entry["relative_path"] for entry in compute_archive_tree(payload)["entries"]] == [
        "nested/a.bin", "z.txt"]


def test_payload_mutation_changes_tree_manifest_and_object(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    before = build_companion_object(payload, store, _metadata())
    (payload / "nested/a.bin").write_bytes(b"changed")
    after = build_companion_object(payload, store, _metadata())
    assert before["archive_tree_sha256"] != after["archive_tree_sha256"]
    assert before["bundle_manifest_sha256"] != after["bundle_manifest_sha256"]
    assert before["object_sha256"] != after["object_sha256"]


def test_snapshot_is_immutable_and_build_uses_exact_captured_bytes(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    snapshot = capture_payload_snapshot(payload)
    with pytest.raises(TypeError):
        snapshot.files["z.txt"] = b"replacement"  # type: ignore[index]
    (payload / "z.txt").write_bytes(b"replacement")
    descriptor = build_companion_object_from_snapshot(snapshot, store, _metadata())
    with tarfile.open(_object_path(store, descriptor), "r:") as archive:
        stream = archive.extractfile("payload/z.txt")
        assert stream is not None
        assert stream.read() == b"z\n"


def test_empty_payload_and_symlinks_are_rejected(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(ValueError):
        compute_archive_tree(empty)
    target = tmp_path / "target"
    target.write_text("x")
    (empty / "link").symlink_to(target)
    with pytest.raises(ValueError):
        build_companion_object(empty, tmp_path / "objects", _metadata())


@pytest.mark.parametrize(("name", "kind", "code"), [
    ("payload/../escape", tarfile.REGTYPE, "CAMPAIGN_TAR_PATH"),
    ("payload/link", tarfile.SYMTYPE, "CAMPAIGN_TAR_TYPE"),
])
def test_rejects_traversal_and_links(tmp_path, name, kind, code):
    raw = _tar([(name, b"", kind)])
    result = validate_companion_object(_store_raw(tmp_path, raw))
    assert not result["valid"]
    assert code in result["failure_codes"]


def test_rejects_duplicate_members(tmp_path):
    raw = _tar([
        ("bundle_manifest.json", b"{}", tarfile.REGTYPE),
        ("bundle_manifest.json", b"{}", tarfile.REGTYPE),
    ])
    result = validate_companion_object(_store_raw(tmp_path, raw))
    assert "CAMPAIGN_TAR_DUPLICATE" in result["failure_codes"]


def test_tar_metadata_tamper_is_rejected(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    descriptor = build_companion_object(payload, store, _metadata())
    members = _members(_object_path(store, descriptor))
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for index, (name, data, kind) in enumerate(members):
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.uid, info.gid, info.mtime, info.type = (
                len(data), 0o644 if index == 1 else 0o600, 0, 0, 0, kind)
            archive.addfile(info, io.BytesIO(data))
    result = validate_companion_object(_store_raw(tmp_path / "tampered", raw.getvalue()))
    assert "CAMPAIGN_TAR_METADATA" in result["failure_codes"]


def test_coherent_manifest_rewrite_changes_external_bindings(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    original = build_companion_object(payload, store, _metadata())
    members = _members(_object_path(store, original))
    manifest = json.loads(members[0][1])
    manifest["campaign_id"] = "campaign-2"
    members[0] = (members[0][0], json.dumps(
        manifest, sort_keys=True, separators=(",", ":")).encode() + b"\n", members[0][2])
    path = _store_raw(tmp_path / "rewritten", _tar(members))
    result = validate_companion_object(path)
    assert result["valid"]
    assert result["descriptor"]["object_sha256"] != original["object_sha256"]
    assert result["descriptor"]["bundle_manifest_sha256"] != original["bundle_manifest_sha256"]
    bound = validate_companion_object(path, original)
    assert "CAMPAIGN_EXPECTED_DESCRIPTOR" in bound["failure_codes"]


def test_wrong_expected_descriptor_is_rejected(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    descriptor = build_companion_object(payload, store, _metadata())
    wrong = dict(descriptor)
    wrong["run_id"] = "other"
    result = validate_companion_object(_object_path(store, descriptor), wrong)
    assert result["failure_codes"] == ["CAMPAIGN_EXPECTED_DESCRIPTOR"]


def test_exclusive_collision_requires_identical_bytes(tmp_path):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    descriptor = build_companion_object(payload, store, _metadata())
    path = _object_path(store, descriptor)
    path.write_bytes(b"not the object")
    with pytest.raises(ValueError, match="collision"):
        build_companion_object(payload, store, _metadata())


def test_walk_error_is_not_silently_omitted(tmp_path, monkeypatch):
    payload = tmp_path / "payload"
    payload.mkdir()

    def failed_scandir(fd):
        raise PermissionError("injected unreadable subtree")

    monkeypatch.setattr(campaign.os, "scandir", failed_scandir)
    with pytest.raises(ValueError, match="subtree is unavailable"):
        compute_archive_tree(payload)


def test_snapshot_rejects_earlier_file_changed_during_later_read(tmp_path, monkeypatch):
    payload = tmp_path / "payload"
    _payload(payload)
    real_open, real_read = campaign.os.open, campaign.os.read
    later_fds = set()
    mutated = False

    def tracked_open(path, flags, mode=0o777, *, dir_fd=None):
        fd = real_open(path, flags, mode, dir_fd=dir_fd)
        if path == "z.txt":
            later_fds.add(fd)
        return fd

    def mutating_read(fd, size):
        nonlocal mutated
        if fd in later_fds and not mutated:
            mutated = True
            (payload / "nested/a.bin").write_bytes(b"changed")
        return real_read(fd, size)

    monkeypatch.setattr(campaign.os, "open", tracked_open)
    monkeypatch.setattr(campaign.os, "read", mutating_read)
    with pytest.raises(ValueError, match="changed during snapshot"):
        compute_archive_tree(payload)


def test_snapshot_rejects_member_added_before_verification_scan(tmp_path, monkeypatch):
    payload = tmp_path / "payload"
    _payload(payload)
    real_scandir = campaign.os.scandir
    root_scans = 0

    def mutating_scandir(fd):
        nonlocal root_scans
        if os.fstat(fd).st_ino == payload.stat().st_ino:
            root_scans += 1
            if root_scans == 2:
                (payload / "added").write_bytes(b"new")
        return real_scandir(fd)

    monkeypatch.setattr(campaign.os, "scandir", mutating_scandir)
    with pytest.raises(ValueError, match="changed during snapshot"):
        compute_archive_tree(payload)


def test_snapshot_rejects_earlier_file_changed_during_verification_scan(tmp_path, monkeypatch):
    payload = tmp_path / "payload"
    _payload(payload)
    real_open = campaign.os.open
    later_opens = 0

    def mutating_open(path, flags, mode=0o777, *, dir_fd=None):
        nonlocal later_opens
        if path == "z.txt":
            later_opens += 1
            if later_opens == 2:
                (payload / "nested/a.bin").write_bytes(b"changed")
        return real_open(path, flags, mode, dir_fd=dir_fd)

    monkeypatch.setattr(campaign.os, "open", mutating_open)
    with pytest.raises(ValueError, match="changed during snapshot"):
        compute_archive_tree(payload)


def test_snapshot_rejects_file_replaced_by_symlink_on_reopen(tmp_path, monkeypatch):
    payload = tmp_path / "payload"
    _payload(payload)
    victim = payload / "nested/a.bin"
    real_open = campaign.os.open
    opens = 0

    def replacing_open(path, flags, mode=0o777, *, dir_fd=None):
        nonlocal opens
        if path == "a.bin":
            opens += 1
            if opens == 2:
                victim.unlink()
                victim.symlink_to(payload / "z.txt")
        return real_open(path, flags, mode, dir_fd=dir_fd)

    monkeypatch.setattr(campaign.os, "open", replacing_open)
    with pytest.raises(ValueError, match="member is unavailable"):
        compute_archive_tree(payload)


def test_metadata_is_independent_during_payload_snapshot(tmp_path, monkeypatch):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    metadata = _metadata()
    original_snapshot = campaign._payload_snapshot

    def mutating_snapshot(root):
        metadata["campaign_id"] = "mutated"
        metadata["evidence_contract"]["id"] = "mutated-contract"
        return original_snapshot(root)

    monkeypatch.setattr(campaign, "_payload_snapshot", mutating_snapshot)
    descriptor = build_companion_object(payload, store, metadata)
    assert descriptor["campaign_id"] == "campaign-1"
    assert descriptor["evidence_contract"]["id"] == "contract-1"


def test_publication_fsyncs_created_directory_chain_and_object(tmp_path, monkeypatch):
    payload, store = tmp_path / "payload", tmp_path / "store-parent" / "objects"
    _payload(payload)
    real_fsync = campaign.os.fsync
    synced = []

    def recording_fsync(fd):
        info = os.fstat(fd)
        synced.append((info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode)))
        real_fsync(fd)

    monkeypatch.setattr(campaign.os, "fsync", recording_fsync)
    descriptor = build_companion_object(payload, store, _metadata())
    object_path = _object_path(store, descriptor)
    expected_directories = [store.parent, store, store / "sha256", object_path.parent]
    for directory in expected_directories:
        info = directory.stat()
        assert (info.st_dev, info.st_ino, stat.S_IFDIR) in synced
    info = object_path.stat()
    assert (info.st_dev, info.st_ino, stat.S_IFREG) in synced


def test_publication_rejects_shard_replaced_before_success(tmp_path, monkeypatch):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)
    descriptor = build_companion_object(payload, store, _metadata())
    object_path = _object_path(store, descriptor)
    shard = object_path.parent
    detached = tmp_path / "detached-shard"

    def replace_shard():
        shard.rename(detached)
        shard.mkdir(mode=0o700)

    monkeypatch.setattr(campaign, "_publication_checkpoint", replace_shard)
    with pytest.raises(ValueError, match="object store changed during publication"):
        build_companion_object(payload, store, _metadata())
    assert not object_path.exists()
    assert (detached / object_path.name).is_file()


def test_reused_publication_fsyncs_linkage_chain_leaf_outward(tmp_path, monkeypatch):
    payload, store = tmp_path / "payload", tmp_path / "store-parent" / "objects"
    _payload(payload)
    descriptor = build_companion_object(payload, store, _metadata())
    object_path = _object_path(store, descriptor)
    expected = [object_path, object_path.parent, store / "sha256", store, store.parent]
    expected_identities = [(path.stat().st_dev, path.stat().st_ino) for path in expected]
    real_fsync = campaign.os.fsync
    synced = []

    def recording_fsync(fd):
        info = os.fstat(fd)
        synced.append((info.st_dev, info.st_ino))
        real_fsync(fd)

    monkeypatch.setattr(campaign.os, "fsync", recording_fsync)
    assert build_companion_object(payload, store, _metadata()) == descriptor
    assert synced == expected_identities


@pytest.mark.parametrize("key", ["campaign_id", "companion_id", "run_id"])
def test_build_rejects_non_scalar_identifier(tmp_path, key):
    payload = tmp_path / "payload"
    _payload(payload)
    with pytest.raises(ValueError, match="identifiers"):
        build_companion_object(payload, tmp_path / "objects", _metadata(**{key: "bad\ud800"}))


@pytest.mark.parametrize("contract_id", ["bad\ud800", "bad\x00id"])
def test_build_rejects_invalid_contract_identifier(tmp_path, contract_id):
    payload = tmp_path / "payload"
    _payload(payload)
    metadata = _metadata(evidence_contract={"id": contract_id, "sha256": H})
    with pytest.raises(ValueError, match="evidence contract"):
        build_companion_object(payload, tmp_path / "objects", metadata)


def test_escaped_lone_surrogate_manifest_returns_invalid_result(tmp_path):
    manifest = b'{"campaign_id":"bad\\ud800"}\n'
    raw = _tar([
        ("bundle_manifest.json", manifest, tarfile.REGTYPE),
        ("payload/x", b"x", tarfile.REGTYPE),
    ])
    result = validate_companion_object(_store_raw(tmp_path, raw))
    assert not result["valid"]
    assert "CAMPAIGN_MANIFEST_UNICODE" in result["failure_codes"]


def test_atomic_write_failure_never_exposes_final_object(tmp_path, monkeypatch):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)

    def failed_write(fd, data):
        raise OSError("injected write failure")

    monkeypatch.setattr(campaign.os, "write", failed_write)
    with pytest.raises(OSError, match="injected write failure"):
        build_companion_object(payload, store, _metadata())
    sha_root = store / "sha256"
    assert not list(sha_root.rglob("*.tar"))
    assert not list(sha_root.rglob("*.tmp"))


def test_atomic_link_failure_cleans_private_object(tmp_path, monkeypatch):
    payload, store = tmp_path / "payload", tmp_path / "objects"
    _payload(payload)

    def failed_link(source, target, *, src_dir_fd, dst_dir_fd, follow_symlinks):
        with pytest.raises(FileNotFoundError):
            os.stat(target, dir_fd=dst_dir_fd, follow_symlinks=False)
        raise OSError("injected link failure")

    monkeypatch.setattr(campaign.os, "link", failed_link)
    with pytest.raises(OSError, match="injected link failure"):
        build_companion_object(payload, store, _metadata())
    sha_root = store / "sha256"
    assert not list(sha_root.rglob("*.tar"))
    assert not list(sha_root.rglob("*.tmp"))


def test_declared_oversized_tar_member_is_rejected_before_read(tmp_path, monkeypatch):
    raw = _tar([
        ("bundle_manifest.json", b"{}", tarfile.REGTYPE),
        ("payload/x", b"x", tarfile.REGTYPE),
    ])
    monkeypatch.setattr(campaign, "_MAX_PAYLOAD_MEMBER_SIZE", 0)
    result = validate_companion_object(_store_raw(tmp_path, raw))
    assert not result["valid"]
    assert "CAMPAIGN_TAR_MEMBER_SIZE" in result["failure_codes"]


def test_payload_stat_limit_rejects_before_read(tmp_path, monkeypatch):
    payload = tmp_path / "payload"
    payload.mkdir()
    (payload / "large").write_bytes(b"xx")
    monkeypatch.setattr(campaign, "_MAX_PAYLOAD_MEMBER_SIZE", 1)
    with pytest.raises(ValueError, match="member size exceeds limit"):
        compute_archive_tree(payload)


FINAL_ROLES = (
    "exact_anchor", "fault_evidence_tamper", "fault_runtime_matrix", "rollback_trace_off")


def _campaign_objects(tmp_path: Path):
    store = tmp_path / "objects"
    descriptors = {}
    for index, role in enumerate(FINAL_ROLES):
        payload = tmp_path / f"payload-{index}"
        payload.mkdir()
        (payload / "tiny.txt").write_text(role)
        descriptors[role] = build_companion_object(payload, store, _metadata(
            role=role, companion_id=f"companion-{index}", run_id=f"run-{index}"))
    return store, descriptors


def _pass_rollback(monkeypatch, calls=None, descriptors=None):
    import mw_perf_collect.rollback_evaluation as rollback

    def passed(path, expected_descriptor):
        if calls is not None:
            calls.append((path, expected_descriptor))
        result = {"valid": True, "failure_codes": [], "descriptor": expected_descriptor}
        if descriptors is not None:
            anchor = descriptors["exact_anchor"]
            result.update({key: anchor[key] for key in (
                "run_id", "root_manifest_sha256", "evidence_manifest_sha256",
                "evidence_contract", "build_identity_sha256")})
        return result

    monkeypatch.setattr(rollback, "validate_trace_off_rollback_companion", passed)


def _pass_specialized(monkeypatch, descriptors):
    def passed(path, expected_descriptor):
        return {
            "valid": True, "failure_codes": [], "descriptor": expected_descriptor,
            "run_id": descriptors["exact_anchor"]["run_id"],
            "root_manifest_sha256": descriptors["exact_anchor"]["root_manifest_sha256"],
            "evidence_manifest_sha256": descriptors["exact_anchor"]["evidence_manifest_sha256"],
            "evidence_contract": descriptors["exact_anchor"]["evidence_contract"],
            "build_identity_sha256": descriptors["exact_anchor"]["build_identity_sha256"],
            "production_build_identity_sha256": descriptors["exact_anchor"]["build_identity_sha256"],
        }
    monkeypatch.setattr(campaign, "validate_exact_anchor_companion", passed)
    import mw_perf_collect.fault_evaluation as fault
    import mw_perf_collect.runtime_fault_evaluation as runtime
    monkeypatch.setattr(fault, "validate_evidence_tamper_companion", passed)
    monkeypatch.setattr(runtime, "validate_runtime_fault_companion", passed)


def test_finalize_campaign_is_deterministic_closed_and_passes_descriptors(tmp_path, monkeypatch):
    store, descriptors = _campaign_objects(tmp_path)
    rollback_calls = []
    generic_calls = []
    real_generic = campaign.validate_companion_object

    def tracked_generic(path, expected_descriptor):
        generic_calls.append((path, expected_descriptor))
        return real_generic(path, expected_descriptor)

    monkeypatch.setattr(campaign, "validate_companion_object", tracked_generic)
    _pass_rollback(monkeypatch, rollback_calls, descriptors)
    _pass_specialized(monkeypatch, descriptors)
    first = finalize_campaign(tmp_path / "final-a", campaign_id="campaign-1",
                              descriptors=descriptors, object_store=store)
    second = finalize_campaign(tmp_path / "final-b", campaign_id="campaign-1",
                               descriptors=descriptors, object_store=store)

    assert {key: first[key] for key in first if key.endswith("sha256")} == {
        key: second[key] for key in second if key.endswith("sha256")}
    assert [item["role"] for item in first["manifest"]["registrations"]] == list(FINAL_ROLES)
    assert [item[1] for item in generic_calls[:4]] == [descriptors[role] for role in FINAL_ROLES]
    assert rollback_calls[0][1] == descriptors["rollback_trace_off"]
    for root in (tmp_path / "final-a", tmp_path / "final-b"):
        assert set(item.name for item in root.iterdir()) == {
            "campaign_manifest.json", "campaign_validation.json", "campaign_seal.json"}
        for item in root.iterdir():
            assert stat.S_IMODE(item.stat().st_mode) == 0o600
            value = json.loads(item.read_bytes())
            assert item.read_bytes() == _canonical_json_bytes(value, newline=True)


@pytest.mark.parametrize(("change", "match"), [
    (lambda values: values.pop("exact_anchor"), "roles must be exact"),
    (lambda values: values.__setitem__("extra", values["exact_anchor"]), "roles must be exact"),
    (lambda values: values["exact_anchor"].__setitem__("role", "fault_runtime_matrix"),
     "role mismatch"),
    (lambda values: values["exact_anchor"].__setitem__("campaign_id", "other"),
     "campaign_id mismatch"),
    (lambda values: values["exact_anchor"].__setitem__(
        "companion_id", values["fault_evidence_tamper"]["companion_id"]), "duplicate"),
    (lambda values: values["exact_anchor"].__setitem__(
        "object_sha256", values["fault_evidence_tamper"]["object_sha256"]), "duplicate"),
])
def test_finalize_campaign_rejects_invalid_registration_set(tmp_path, monkeypatch, change, match):
    store, originals = _campaign_objects(tmp_path)
    descriptors = {role: dict(value) for role, value in originals.items()}
    change(descriptors)
    _pass_rollback(monkeypatch, descriptors=descriptors)
    with pytest.raises(ValueError, match=match):
        finalize_campaign(tmp_path / "final", campaign_id="campaign-1",
                          descriptors=descriptors, object_store=store)
    assert not (tmp_path / "final").exists()


def test_finalize_campaign_rejects_tampered_object_and_existing_destination(tmp_path, monkeypatch):
    store, descriptors = _campaign_objects(tmp_path)
    _pass_rollback(monkeypatch, descriptors=descriptors)
    _pass_specialized(monkeypatch, descriptors)
    path = _object_path(store, descriptors["fault_runtime_matrix"])
    path.write_bytes(b"tampered")
    with pytest.raises(ValueError, match="validation failed"):
        finalize_campaign(tmp_path / "final", campaign_id="campaign-1",
                          descriptors=descriptors, object_store=store)
    existing = tmp_path / "existing"
    existing.mkdir()
    with pytest.raises(FileExistsError):
        finalize_campaign(existing, campaign_id="campaign-1",
                          descriptors=descriptors, object_store=store)


def test_finalize_campaign_publication_failure_cleans_staging(tmp_path, monkeypatch):
    store, descriptors = _campaign_objects(tmp_path)
    _pass_rollback(monkeypatch, descriptors=descriptors)
    _pass_specialized(monkeypatch, descriptors)

    def failed_replace(source, destination):
        raise OSError("injected campaign publication failure")

    monkeypatch.setattr(campaign, "_rename_noreplace", failed_replace)
    root = tmp_path / "final"
    with pytest.raises(OSError, match="injected campaign publication failure"):
        finalize_campaign(root, campaign_id="campaign-1",
                          descriptors=descriptors, object_store=store)
    assert not root.exists()
    assert not list(tmp_path.glob(".final.*.staging"))


def test_finalize_campaign_never_replaces_racing_destination(tmp_path, monkeypatch):
    store, descriptors = _campaign_objects(tmp_path)
    _pass_rollback(monkeypatch, descriptors=descriptors)
    _pass_specialized(monkeypatch, descriptors)
    root = tmp_path / "final"
    real_rename = campaign._rename_noreplace

    def race(source, destination):
        destination.mkdir()
        (destination / "owner").write_text("external")
        real_rename(source, destination)

    monkeypatch.setattr(campaign, "_rename_noreplace", race)
    with pytest.raises(FileExistsError):
        finalize_campaign(root, campaign_id="campaign-1",
                          descriptors=descriptors, object_store=store)
    assert (root / "owner").read_text() == "external"
    assert not list(tmp_path.glob(".final.*.staging"))


def test_campaign_schemas_are_closed(tmp_path):
    schemas = Path(campaign.__file__).parent / "schemas"
    for name in ("evidence-campaign.schema.v1.json",
                 "evidence-campaign-validation.schema.v1.json",
                 "evidence-campaign-seal.schema.v1.json"):
        schema = json.loads((schemas / name).read_bytes())
        assert schema["additionalProperties"] is False
        minimal: dict[str, Any] = {key: None for key in schema["required"]}
        minimal["unexpected"] = True
        assert any(error.endswith(":additional") for error in _schema_errors(minimal, schema))
