import hashlib
import json
import os
import tarfile
from pathlib import Path

import pytest

from mw_perf_collect import core, fault_evaluation
from mw_perf_collect.campaign import (
    PayloadSnapshot,
    build_companion_object_from_snapshot,
    validate_companion_object,
)
from mw_perf_collect.core import _canonical_json_bytes, _schema_errors
from mw_perf_collect.fault_evaluation import (
    build_evidence_tamper_companion,
    evaluate_evidence_tamper,
    validate_evidence_tamper_companion,
)


TARGET = "capability/hook_callgraph.json"
EXPECTED = "P0A_HOOK_CALLGRAPH"
VERSION = "mw_perf.p0a_contract.v8"


def _anchor(root: Path, *, target: str = TARGET, target_data: bytes = b"abcdef") -> Path:
    anchor = root / "anchor"
    (anchor / Path(target).parent).mkdir(parents=True)
    (anchor / "registration").mkdir()
    (anchor / "misc/empty").mkdir(parents=True)
    (anchor / target).write_bytes(target_data)
    _contract, contract_raw = core._trusted_p0a_contract(VERSION)
    (anchor / "registration/contract.json").write_bytes(contract_raw)
    evidence = {
        "schema": "mw_perf.p0a_evidence_manifest.v1",
        "validation_profile": "p0a",
        "run_id": "run-1",
        "registration_contract_ref": "registration/contract.json",
        "registration_contract_version": VERSION,
        "registration_contract_sha256": hashlib.sha256(contract_raw).hexdigest(),
        "entries": [{
            "relative_path": target,
            "sha256": hashlib.sha256(target_data).hexdigest(),
            "status": "required",
        }],
    }
    (anchor / "capability").mkdir(exist_ok=True)
    (anchor / "capability/p0a_evidence_manifest.json").write_bytes(
        _canonical_json_bytes(evidence, newline=True))
    manifest = {
        "run_id": "run-1",
        "run_status": "completed",
        "candidate_only": True,
        "metric_availability": "unsupported",
        "valid_for_comparison": False,
        "dds_comm_trace": {"registration_contract_version": VERSION},
    }
    (anchor / "manifest.json").write_bytes(_canonical_json_bytes(manifest, newline=True))
    (anchor / "misc/note.txt").write_bytes(b"complete copy")
    return anchor


def _validator(original: bytes = b"abcdef", *, nested_expected: bool = True):
    def validate(path: Path, *, profile: str):
        assert profile == "p0a"
        data = (path / TARGET).read_bytes()
        nested = ["P0A_ARTIFACT"]
        if data != original and nested_expected:
            nested.append(EXPECTED)
        return {
            "valid_for_collection": True,
            "valid_for_metric": False,
            "valid_for_comparison": False,
            "failure_codes": ["CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"],
            "p0a_evidence": {"valid": False, "failure_codes": nested},
        }
    return validate


def _evaluate(tmp_path: Path, **kwargs):
    anchor = _anchor(tmp_path)
    experiment = tmp_path / "experiment"
    report = evaluate_evidence_tamper(anchor, experiment, TARGET,
                                      validator=_validator(), **kwargs)
    return anchor, experiment, report


def _object_path(store: Path, descriptor: dict) -> Path:
    digest = descriptor["object_sha256"]
    return store / "sha256" / digest[:2] / f"{digest}.tar"


def _archived_payloads(path: Path) -> dict[str, bytes]:
    with tarfile.open(path, "r:") as archive:
        payloads = {}
        for member in archive:
            if member.isreg() and member.name.startswith("payload/"):
                stream = archive.extractfile(member)
                assert stream is not None
                payloads[member.name.removeprefix("payload/")] = stream.read()
        return payloads


def _rebuild_payload(store: Path, descriptor: dict, payloads: dict[str, bytes],
                     **metadata_changes) -> tuple[Path, dict]:
    metadata = {key: descriptor[key] for key in (
        "campaign_id", "role", "companion_id", "run_id", "evidence_contract",
        "root_manifest_sha256", "evidence_manifest_sha256", "build_identity_sha256")}
    metadata.update(metadata_changes)
    rebuilt = build_companion_object_from_snapshot(PayloadSnapshot(payloads), store, metadata)
    return _object_path(store, rebuilt), rebuilt


def test_one_byte_mutation_report_and_source_unchanged(tmp_path):
    anchor, experiment, report = _evaluate(tmp_path, mutation_offset=2)
    source = (anchor / TARGET).read_bytes()
    changed = (experiment / "payload/anchor_snapshot" / TARGET).read_bytes()
    assert source == b"abcdef"
    assert len(changed) == len(source)
    assert changed == source[:2] + bytes([source[2] ^ 1]) + source[3:]
    assert report["mutation"]["size"] == 6
    assert report["mutation"]["offset"] == 2
    assert report["validation"]["expected_new_p0a_code"] == EXPECTED
    assert report["archive_tree_after_sha256"] != report["anchor"]["archive_tree_before_sha256"]
    assert (experiment / "payload/anchor_snapshot/misc/empty").is_dir()


def test_candidate_baseline_nested_code_and_new_target_code_are_required(tmp_path):
    anchor = _anchor(tmp_path)
    with pytest.raises(ValueError, match="target-specific"):
        evaluate_evidence_tamper(anchor, tmp_path / "failed", TARGET,
                                 validator=_validator(nested_expected=False))
    assert not (tmp_path / "failed").exists()

    def no_nested_artifact(path, *, profile):
        result = _validator()(path, profile=profile)
        if path.name == "anchor_snapshot" and (path / TARGET).read_bytes() == b"abcdef":
            result["p0a_evidence"]["failure_codes"] = []
        return result

    with pytest.raises(ValueError, match="baseline lacks nested"):
        evaluate_evidence_tamper(anchor, tmp_path / "failed-2", TARGET,
                                 validator=no_nested_artifact)


def test_unallowlisted_target_and_existing_experiment_are_rejected(tmp_path):
    anchor = _anchor(tmp_path)
    with pytest.raises(ValueError, match="allowlisted"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", "manifest.json",
                                 validator=_validator())
    existing = tmp_path / "existing"
    existing.mkdir()
    with pytest.raises(ValueError, match="must not already exist"):
        evaluate_evidence_tamper(anchor, existing, TARGET, validator=_validator())


def test_stale_evidence_hash_is_rejected_and_cleaned(tmp_path):
    anchor = _anchor(tmp_path)
    evidence_path = anchor / "capability/p0a_evidence_manifest.json"
    evidence = json.loads(evidence_path.read_text())
    evidence["entries"][0]["sha256"] = "0" * 64
    evidence_path.write_bytes(_canonical_json_bytes(evidence, newline=True))
    experiment = tmp_path / "experiment"
    with pytest.raises(ValueError, match="stale"):
        evaluate_evidence_tamper(anchor, experiment, TARGET, validator=_validator())
    assert not experiment.exists()


@pytest.mark.parametrize("kind", ["symlink", "fifo"])
def test_symlink_and_special_members_are_rejected(tmp_path, kind):
    anchor = _anchor(tmp_path)
    member = anchor / "bad"
    if kind == "symlink":
        member.symlink_to(anchor / TARGET)
    else:
        os.mkfifo(member)
    with pytest.raises(ValueError, match="symlink or special"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=_validator())
    assert not (tmp_path / "experiment").exists()


def test_source_mutation_during_validation_is_detected(tmp_path):
    anchor = _anchor(tmp_path)
    calls = 0

    def mutate_source(path, *, profile):
        nonlocal calls
        calls += 1
        result = _validator()(path, profile=profile)
        if calls == 2:
            (anchor / "misc/note.txt").write_bytes(b"changed source")
        return result

    with pytest.raises(ValueError, match="anchor changed"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=mutate_source)
    assert not (tmp_path / "experiment").exists()


def test_existing_seal_anywhere_is_rejected(tmp_path):
    anchor = _anchor(tmp_path)
    (anchor / "misc/deep").mkdir()
    (anchor / "misc/deep/campaign_seal.json").write_text("{}")
    with pytest.raises(ValueError, match="seal"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=_validator())


def test_final_seal_name_is_rejected(tmp_path):
    anchor = _anchor(tmp_path)
    (anchor / "misc/deep").mkdir()
    (anchor / "misc/deep/campaign_final_seal.json").write_text("{}")
    with pytest.raises(ValueError, match="seal"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=_validator())


@pytest.mark.parametrize("offset", [-1, 6, 100])
def test_offset_must_be_nonnegative_and_inside_nonempty_target(tmp_path, offset):
    anchor = _anchor(tmp_path)
    with pytest.raises(ValueError, match="mutation_offset"):
        evaluate_evidence_tamper(anchor, tmp_path / f"experiment-{offset}", TARGET,
                                 mutation_offset=offset, validator=_validator())


def test_zero_size_target_is_rejected(tmp_path):
    anchor = _anchor(tmp_path, target_data=b"")
    with pytest.raises(ValueError, match="mutation_offset"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=_validator(b""))


def test_report_is_canonical_closed_and_has_no_paths(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    report_path = experiment / "payload/fault_evidence_tamper.json"
    assert report_path.read_bytes() == _canonical_json_bytes(report, newline=True)
    schema = json.loads((Path(__file__).parents[1] /
        "schemas/fault-evidence-tamper-result.schema.v1.json").read_text())
    assert _schema_errors(report, schema) == []
    changed = dict(report)
    changed["path"] = "/forbidden"
    assert _schema_errors(changed, schema)
    serialized = json.dumps(report)
    assert str(tmp_path) not in serialized
    assert '"path"' not in serialized


def test_missing_comparison_flag_is_allowed_but_true_is_rejected(tmp_path):
    anchor = _anchor(tmp_path)
    manifest_path = anchor / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    del manifest["valid_for_comparison"]
    manifest_path.write_bytes(_canonical_json_bytes(manifest, newline=True))
    report = evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                      validator=_validator())
    assert report["campaign_classification"]["valid_for_comparison"] is False

    anchor = _anchor(tmp_path / "second")
    manifest_path = anchor / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["valid_for_comparison"] = True
    manifest_path.write_bytes(_canonical_json_bytes(manifest, newline=True))
    with pytest.raises(ValueError, match="completed unsupported candidate"):
        evaluate_evidence_tamper(anchor, tmp_path / "failed", TARGET,
                                 validator=_validator())


def test_real_candidate_manifest_shape_allows_absent_flag_and_rejects_true(tmp_path):
    manifest_path = _anchor(tmp_path) / "manifest.json"
    root = json.loads(manifest_path.read_bytes())
    del root["valid_for_comparison"]
    root_raw = _canonical_json_bytes(root, newline=True)
    root = json.loads(root_raw)
    assert "valid_for_comparison" not in root
    version = root["dds_comm_trace"]["registration_contract_version"]
    _contract, contract_raw = core._trusted_p0a_contract(version)
    target_raw = b"real-shape-target"
    evidence = {
        "run_id": root["run_id"],
        "registration_contract_ref": "registration/contract.json",
        "registration_contract_version": version,
        "registration_contract_sha256": hashlib.sha256(contract_raw).hexdigest(),
        "entries": [{"relative_path": TARGET, "status": "required",
                     "sha256": hashlib.sha256(target_raw).hexdigest()}],
    }
    metadata = fault_evaluation._baseline_metadata_bytes(
        root_raw, _canonical_json_bytes(evidence, newline=True), contract_raw,
        target_raw, TARGET)
    assert metadata["run_id"] == root["run_id"]

    root["valid_for_comparison"] = True
    with pytest.raises(ValueError, match="completed unsupported candidate"):
        fault_evaluation._baseline_metadata_bytes(
            _canonical_json_bytes(root, newline=True),
            _canonical_json_bytes(evidence, newline=True), contract_raw,
            target_raw, TARGET)


def test_absolute_path_validator_code_is_rejected(tmp_path):
    anchor = _anchor(tmp_path)

    def invalid_code(path, *, profile):
        result = _validator()(path, profile=profile)
        result["failure_codes"].append("/tmp/private/result")
        return result

    with pytest.raises(ValueError, match="failure codes"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=invalid_code)


def test_structural_baseline_p0a_failure_is_rejected(tmp_path):
    anchor = _anchor(tmp_path)

    def structural_failure(path, *, profile):
        result = _validator()(path, profile=profile)
        if (path / TARGET).read_bytes() == b"abcdef":
            result["p0a_evidence"]["failure_codes"].append("P0A_CONTRACT_HASH")
        return result

    with pytest.raises(ValueError, match="forbidden nested"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=structural_failure)


def test_source_replacement_during_copied_baseline_is_rejected(tmp_path):
    anchor = _anchor(tmp_path)

    def replace_source(path, *, profile):
        result = _validator()(path, profile=profile)
        replacement = tmp_path / "replacement"
        anchor.rename(tmp_path / "old-anchor")
        replacement.mkdir()
        replacement.rename(anchor)
        return result

    with pytest.raises(ValueError, match="anchor changed"):
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 validator=replace_source)


def test_cleanup_failure_is_distinct_and_chained(tmp_path, monkeypatch):
    anchor = _anchor(tmp_path)

    def fail_cleanup(path):
        raise OSError("cleanup denied")

    monkeypatch.setattr(fault_evaluation.shutil, "rmtree", fail_cleanup)
    with pytest.raises(RuntimeError, match="cleanup failed") as caught:
        evaluate_evidence_tamper(anchor, tmp_path / "experiment", TARGET,
                                 mutation_offset=-1, validator=_validator())
    assert isinstance(caught.value.__cause__, ValueError)
    assert not (tmp_path / "experiment/payload/fault_evidence_tamper.json").exists()


def test_companion_packages_complete_experiment_and_validates(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    store = tmp_path / "objects"
    descriptor = build_evidence_tamper_companion(
        report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
        build_identity_sha256="d" * 64, object_store=store)
    assert descriptor["role"] == "fault_evidence_tamper"
    assert descriptor["run_id"] == report["anchor"]["run_id"]
    assert descriptor["evidence_contract"] == report["anchor"]["registration_contract"]
    validated = validate_companion_object(_object_path(store, descriptor), descriptor)
    assert validated["valid"]
    assert validated["bundle_manifest"]["payload_member_count"] > 1
    specialized = validate_evidence_tamper_companion(
        _object_path(store, descriptor), descriptor)
    assert specialized["valid"]
    assert {key: specialized[key] for key in (
        "run_id", "root_manifest_sha256", "evidence_manifest_sha256",
        "build_identity_sha256", "evidence_contract")} == {
            "run_id": descriptor["run_id"],
            "root_manifest_sha256": descriptor["root_manifest_sha256"],
            "evidence_manifest_sha256": descriptor["evidence_manifest_sha256"],
            "build_identity_sha256": descriptor["build_identity_sha256"],
            "evidence_contract": descriptor["evidence_contract"],
        }


def test_specialized_companion_requires_expected_descriptor(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    store = tmp_path / "objects"
    descriptor = build_evidence_tamper_companion(
        report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
        build_identity_sha256="d" * 64, object_store=store)
    result = validate_evidence_tamper_companion(_object_path(store, descriptor))
    assert result["valid"] is False
    assert result["failure_codes"] == ["FAULT_EVIDENCE_TAMPER_EXPECTED_DESCRIPTOR"]


def test_specialized_companion_rejects_tampered_tar_bytes(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    store = tmp_path / "objects"
    descriptor = build_evidence_tamper_companion(
        report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
        build_identity_sha256="d" * 64, object_store=store)
    object_path = _object_path(store, descriptor)
    raw = bytearray(object_path.read_bytes())
    raw[600] ^= 1
    object_path.write_bytes(raw)
    assert not validate_evidence_tamper_companion(object_path, descriptor)["valid"]


def test_specialized_companion_rejects_coherent_anchor_rewrite(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    store = tmp_path / "objects"
    descriptor = build_evidence_tamper_companion(
        report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
        build_identity_sha256="d" * 64, object_store=store)
    payloads = _archived_payloads(_object_path(store, descriptor))
    rewritten = json.loads(payloads["fault_evidence_tamper.json"])
    rewritten["anchor"]["run_id"] = "rewritten-run"
    payloads["fault_evidence_tamper.json"] = _canonical_json_bytes(rewritten, newline=True)
    path, coherent = _rebuild_payload(store, descriptor, payloads, run_id="rewritten-run")
    assert validate_companion_object(path, coherent)["valid"]
    result = validate_evidence_tamper_companion(path, coherent)
    assert result["failure_codes"] == ["FAULT_EVIDENCE_TAMPER_OBJECT_SEMANTICS"]


def test_specialized_companion_rejects_coherent_two_byte_rewrite(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path, mutation_offset=2)
    store = tmp_path / "objects"
    descriptor = build_evidence_tamper_companion(
        report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
        build_identity_sha256="d" * 64, object_store=store)
    payloads = _archived_payloads(_object_path(store, descriptor))
    target_name = "anchor_snapshot/" + TARGET
    forged = bytearray(payloads[target_name])
    forged[4] ^= 1
    payloads[target_name] = bytes(forged)
    rewritten = json.loads(payloads["fault_evidence_tamper.json"])
    rewritten["mutation"]["after_sha256"] = hashlib.sha256(forged).hexdigest()
    snapshot = {name.removeprefix("anchor_snapshot/"): data
                for name, data in payloads.items() if name.startswith("anchor_snapshot/")}
    rewritten["archive_tree_after_sha256"] = fault_evaluation.compute_snapshot_archive_tree(
        PayloadSnapshot(snapshot))["sha256"]
    payloads["fault_evidence_tamper.json"] = _canonical_json_bytes(rewritten, newline=True)
    path, coherent = _rebuild_payload(store, descriptor, payloads)
    assert validate_companion_object(path, coherent)["valid"]
    result = validate_evidence_tamper_companion(path, coherent)
    assert result["failure_codes"] == ["FAULT_EVIDENCE_TAMPER_OBJECT_SEMANTICS"]


def test_companion_rejects_rewritten_report_even_when_argument_matches(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    rewritten = json.loads(json.dumps(report))
    rewritten["anchor"]["run_id"] = "rewritten-run"
    (experiment / "payload/fault_evidence_tamper.json").write_bytes(
        _canonical_json_bytes(rewritten, newline=True))
    with pytest.raises(ValueError, match="mutated snapshot"):
        build_evidence_tamper_companion(
            rewritten, experiment, campaign_id="campaign-1", companion_id="tamper-1",
            build_identity_sha256="d" * 64, object_store=tmp_path / "objects")


@pytest.mark.parametrize("extra", ["unexpected.txt", "campaign_seal.json", "deep/final_seal.json"])
def test_companion_rejects_extra_payload_member_or_seal(tmp_path, extra):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    extra_path = experiment / "payload" / extra
    extra_path.parent.mkdir(parents=True, exist_ok=True)
    extra_path.write_text("unexpected")
    with pytest.raises(ValueError, match="unexpected member or final seal"):
        build_evidence_tamper_companion(
            report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
            build_identity_sha256="d" * 64, object_store=tmp_path / "objects")


def test_companion_rejects_untrusted_archived_contract(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    contract_path = experiment / "payload/anchor_snapshot/registration/contract.json"
    contract = json.loads(contract_path.read_text())
    contract["registration_contract_version"] = VERSION
    contract_path.write_bytes(_canonical_json_bytes(contract, newline=False) + b" ")
    with pytest.raises(ValueError, match="contract"):
        build_evidence_tamper_companion(
            report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
            build_identity_sha256="d" * 64, object_store=tmp_path / "objects")


def test_companion_rejects_coherent_two_byte_forgery(tmp_path):
    _anchor_path, experiment, report = _evaluate(tmp_path, mutation_offset=2)
    target_path = experiment / "payload/anchor_snapshot" / TARGET
    forged = bytearray(target_path.read_bytes())
    forged[4] ^= 1
    target_path.write_bytes(forged)
    report["mutation"]["after_sha256"] = hashlib.sha256(forged).hexdigest()
    report["archive_tree_after_sha256"] = fault_evaluation.compute_archive_tree(
        experiment / "payload/anchor_snapshot")["sha256"]
    (experiment / "payload/fault_evidence_tamper.json").write_bytes(
        _canonical_json_bytes(report, newline=True))
    with pytest.raises(ValueError, match="mutated snapshot"):
        build_evidence_tamper_companion(
            report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
            build_identity_sha256="d" * 64, object_store=tmp_path / "objects")


def test_companion_packages_verified_snapshot_when_path_is_replaced(tmp_path, monkeypatch):
    _anchor_path, experiment, report = _evaluate(tmp_path)
    report_path = experiment / "payload/fault_evidence_tamper.json"
    verified_report = report_path.read_bytes()
    capture = fault_evaluation.capture_payload_snapshot

    def capture_then_replace(path):
        snapshot = capture(path)
        report_path.write_bytes(b"{}")
        return snapshot

    monkeypatch.setattr(fault_evaluation, "capture_payload_snapshot", capture_then_replace)
    store = tmp_path / "objects"
    descriptor = build_evidence_tamper_companion(
        report, experiment, campaign_id="campaign-1", companion_id="tamper-1",
        build_identity_sha256="d" * 64, object_store=store)
    with tarfile.open(_object_path(store, descriptor), "r:") as archive:
        stream = archive.extractfile("payload/fault_evidence_tamper.json")
        assert stream is not None
        assert stream.read() == verified_report
