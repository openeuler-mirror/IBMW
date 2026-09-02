import copy
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

from mw_perf_collect import core
from mw_perf_collect import metric_qualification as qualification
from mw_perf_collect.cli import main


H = "a" * 64
ROOT = "b" * 64
PROVIDER = "test-provider"
CAMPAIGN_ID = "campaign"


def _registry(tmp_path, *, root=ROOT, provider=PROVIDER, campaign_id=CAMPAIGN_ID):
    tmp_path.mkdir(parents=True, exist_ok=True)
    path = tmp_path / "trusted-roots.json"
    value = {"schema": "mw_perf.trusted_external_roots.v1", "entries": [{
        "provider": provider, "campaign_id": campaign_id,
        "external_root_sha256": root,
        "operator_approval": {"approved": True, "approved_by": "test-operator",
                              "approved_at": "2026-08-27T00:00:00Z",
                              "approval_reference": "test-only"}}]}
    raw = core._canonical_json_bytes(value, newline=True)
    path.write_bytes(raw)
    return path, hashlib.sha256(raw).hexdigest()


def _qualification_args(registry, registry_hash, *, provider=PROVIDER,
                        campaign_id=CAMPAIGN_ID):
    return {"registry_path": registry, "expected_registry_sha256": registry_hash,
            "provider": provider, "campaign_id": campaign_id}


@pytest.fixture
def noncanonical_historical_source_json():
    return b'{\n  "value": 1,\n  "schema": "historical-v8"\n}\n'


def _descriptor(contract):
    return {"campaign_id": "campaign", "role": "exact_anchor",
            "companion_id": contract, "object_sha256": H,
            "bundle_manifest_sha256": H, "archive_tree_sha256": H,
            "run_id": "run", "evidence_contract": {"id": contract, "sha256": H},
            "root_manifest_sha256": H, "evidence_manifest_sha256": H,
            "build_identity_sha256": H}


def _source():
    sample = core._canonical_json_bytes({
        "schema": "mw_perf.latency_sample.v2", "latency_ns": 10,
        "publish_start_ns": 10, "callback_start_ns": 20}, newline=True)
    sample_hash = hashlib.sha256(sample).hexdigest()
    formal = {"formal_anchor_id": H}
    return {"manifest": {"campaign_id": "campaign"},
        "formal_descriptor": _descriptor("mw_perf.p0a_contract.v9"),
        "formal": formal, "source_descriptor": _descriptor("mw_perf.p0a_contract.v8"),
        "latency": {"content_sha256": H},
        "delivery": {"content_sha256": H, "delivery_set_sha256": H,
                     "observed_callback_count": 1},
        "quality": {"content_sha256": H, "sample_set_sha256": H,
                    "delivery_set_sha256": H, "stage_record_count": 3,
                    "exact_record_count": 1},
        "samples": {"bytes": sample, "content_sha256": sample_hash,
                    "record_count": 1},
        "identity": {"source_set_sha256": H}}


def _install_source(monkeypatch, source=None):
    current = source or _source()

    def derive(_campaign, _store, root):
        if root != ROOT:
            raise ValueError("external root mismatch")
        return copy.deepcopy(current)

    monkeypatch.setattr(qualification, "_source_derivation", derive)
    return current


def _build(tmp_path, monkeypatch):
    source = _install_source(monkeypatch)
    tmp_path.mkdir(parents=True, exist_ok=True)
    campaign = tmp_path / "campaign"; campaign.mkdir()
    store = tmp_path / "objects"; store.mkdir()
    before = {path: path.read_bytes() for path in (campaign, store) if path.is_file()}
    destination = tmp_path / "qualification"
    registry, registry_hash = _registry(tmp_path)
    result = qualification.build_metric_qualification(
        campaign, object_store=store, **_qualification_args(registry, registry_hash),
        destination=destination)
    assert before == {path: path.read_bytes() for path in (campaign, store) if path.is_file()}
    return destination, campaign, store, registry, registry_hash, result, source


def _rewrite_json(path, mutate):
    path.parent.chmod(0o700); path.chmod(0o600)
    value = json.loads(path.read_bytes()); mutate(value)
    path.write_bytes(core._canonical_json_bytes(value, newline=True))


def test_historical_v8_json_accepts_format_only_after_campaign_validation(
        noncanonical_historical_source_json):
    relative = "capability/p0a_evidence_manifest.json"
    payloads = {relative: noncanonical_historical_source_json}
    assert qualification._historical_v8_json(payloads, relative) == {
        "schema": "historical-v8", "value": 1}
    with pytest.raises(ValueError, match="not canonical JSON"):
        qualification._json(payloads, relative)


@pytest.mark.parametrize("raw", [
    b'{"schema":"historical-v8","schema":"duplicate"}\n',
    b'{"schema":"historical-v8","value":NaN}\n',
    b'{"schema":"historical-v8","value":Infinity}\n',
])
def test_historical_v8_json_remains_strict(raw):
    relative = "capability/p0a_evidence_manifest.json"
    with pytest.raises((ValueError, json.JSONDecodeError)):
        qualification._historical_v8_json({relative: raw}, relative)


@pytest.mark.parametrize("raw", [
    b'{"schema":"mw_perf.formal_anchor.v1","schema":"duplicate"}\n',
    b'{"schema":"mw_perf.formal_anchor.v1","value":NaN}\n',
])
def test_qualification_formal_json_is_strict(raw):
    with pytest.raises((ValueError, json.JSONDecodeError)):
        qualification._json({qualification.formal_anchor._FORMAL: raw},
                            qualification.formal_anchor._FORMAL)


def test_build_validate_is_atomic_content_derived_and_non_comparable(tmp_path, monkeypatch):
    destination, campaign, store, registry, registry_hash, result, _source_value = _build(tmp_path, monkeypatch)
    checked = qualification.validate_metric_qualification(
        destination, campaign_root=campaign, object_store=store,
        **_qualification_args(registry, registry_hash))
    assert checked["valid"] and checked["valid_for_metric"]
    assert checked["metric_availability"] == "supported"
    assert checked["valid_for_comparison"] is False
    assert result["qualification_id"] == checked["qualification"]["qualification_id"]
    assert set(path.name for path in destination.iterdir()) == qualification._FILES
    assert stat.S_IMODE(destination.stat().st_mode) == 0o500
    assert all(stat.S_IMODE(path.stat().st_mode) == 0o400
               for path in destination.iterdir())
    for name in ("declaration.json", "metric_qualification.json",
                 "qualification_seal.json"):
        raw = (destination / name).read_bytes()
        assert raw == core._canonical_json_bytes(json.loads(raw), newline=True)
    text = (destination / "metric_qualification.json").read_text()
    assert result["qualification_id"] not in json.dumps(
        checked["qualification"]["source_campaign"])
    assert all(word not in text.lower() for word in ("percentile_value", "threshold_value",
        "overhead_value", "ranking_value", "regression_value", "acceptance_passed"))


@pytest.mark.parametrize(("field", "mutate"), [
    ("formal", lambda v: v["formal_anchor"].update(content_sha256="0" * 64)),
    ("raw", lambda v: v["source_artifacts"].update(raw_source_set_sha256="0" * 64)),
    ("sample", lambda v: v["latency_samples"].update(content_sha256="0" * 64)),
    ("delivery", lambda v: v["delivery_binding"].update(delivery_set_sha256="0" * 64)),
    ("trace", lambda v: v["trace_quality_binding"].update(exact_record_count=2)),
    ("capability", lambda v: v.update(capability_id="other")),
    ("boundary", lambda v: v["metric"].update(start_boundary="other.start")),
    ("unit", lambda v: v["metric"].update(unit="us")),
    ("clock", lambda v: v["metric"].update(clock_domain="wall")),
    ("count", lambda v: v["latency_samples"].update(record_count=2)),
])
def test_validator_rejects_qualification_field_tamper(
        tmp_path, monkeypatch, field, mutate):
    destination, campaign, store, registry, registry_hash, _result, _source_value = _build(tmp_path, monkeypatch)
    _rewrite_json(destination / "metric_qualification.json", mutate)
    checked = qualification.validate_metric_qualification(
        destination, campaign_root=campaign, object_store=store,
        **_qualification_args(registry, registry_hash))
    assert not checked["valid"], field
    assert checked["valid_for_metric"] is False


def test_validator_rejects_root_samples_and_coherent_rewrite(tmp_path, monkeypatch):
    destination, campaign, store, registry, registry_hash, _result, _source_value = _build(tmp_path, monkeypatch)
    assert not qualification.validate_metric_qualification(
        destination, campaign_root=campaign, object_store=store,
        **_qualification_args(registry, "0" * 64))["valid"]
    samples = destination / "latency_samples.jsonl"
    destination.chmod(0o700); samples.chmod(0o600)
    samples.write_bytes(samples.read_bytes() + b"{}\n")
    assert not qualification.validate_metric_qualification(
        destination, campaign_root=campaign, object_store=store,
        **_qualification_args(registry, registry_hash))["valid"]

    destination, campaign, store, registry, registry_hash, _result, _source_value = _build(
        tmp_path / "coherent", monkeypatch)
    _rewrite_json(destination / "metric_qualification.json",
                  lambda value: value["classification"].update(valid_for_comparison=True))
    document = (destination / "metric_qualification.json").read_bytes()
    declaration = (destination / "declaration.json").read_bytes()
    samples = (destination / "latency_samples.jsonl").read_bytes()
    seal_path = destination / "qualification_seal.json"
    seal_path.chmod(0o600)
    seal = json.loads(seal_path.read_bytes())
    seal["metric_qualification_sha256"] = hashlib.sha256(document).hexdigest()
    seal["artifact_sha256"] = hashlib.sha256(
        qualification._ARTIFACT_DOMAIN + declaration + samples + document).hexdigest()
    seal_path.write_bytes(core._canonical_json_bytes(seal, newline=True))
    assert not qualification.validate_metric_qualification(
        destination, campaign_root=campaign, object_store=store,
        **_qualification_args(registry, registry_hash))["valid"]


def test_failed_build_leaves_no_destination_or_staging(tmp_path, monkeypatch):
    _install_source(monkeypatch)
    campaign = tmp_path / "campaign"; campaign.mkdir()
    store = tmp_path / "objects"; store.mkdir()
    registry, registry_hash = _registry(tmp_path)
    monkeypatch.setattr(qualification, "validate_metric_qualification",
                        lambda *args, **kwargs: {"valid": False})
    with pytest.raises(ValueError):
        qualification.build_metric_qualification(
            campaign, object_store=store,
            **_qualification_args(registry, registry_hash),
            destination=tmp_path / "qualification")
    assert not (tmp_path / "qualification").exists()
    assert not list(tmp_path.glob(".qualification.*.staging"))


def test_compare_rejects_qualification_and_install_metadata_is_complete(
        tmp_path, monkeypatch):
    destination, _campaign, _store, _registry_path, _registry_hash, _result, _source_value = _build(tmp_path, monkeypatch)
    plan = tmp_path / "plan.json"
    plan.write_text(json.dumps({"runs": [str(destination)]}))
    output = tmp_path / "comparison"
    assert main(["compare", "--plan", str(plan), "--output", str(output)]) != 0
    compared = json.loads((output / "comparison.json").read_bytes())
    assert compared["valid_for_comparison"] is False
    cmake = (qualification.Path(qualification.__file__).parent / "CMakeLists.txt").read_text()
    assert "metric_qualification.py" in cmake
    assert (qualification.Path(qualification.__file__).parent / "schemas" /
            "metric-qualification.schema.v1.json").is_file()
    assert (qualification.Path(qualification.__file__).parent / "contracts" /
            "metric-qualification-contract.v1.json").is_file()


def test_registry_loader_rejects_wrong_hash_and_identity(tmp_path):
    registry, registry_hash = _registry(tmp_path)
    for changed_hash, provider, campaign_id in (
            ("0" * 64, PROVIDER, CAMPAIGN_ID),
            (registry_hash, "wrong-provider", CAMPAIGN_ID),
            (registry_hash, PROVIDER, "wrong-campaign")):
        with pytest.raises(ValueError):
            qualification.load_trusted_external_root(
                registry, expected_registry_sha256=changed_hash,
                provider=provider, campaign_id=campaign_id)


def test_post_rename_race_removes_only_owned_destination(tmp_path, monkeypatch):
    _install_source(monkeypatch)
    campaign_root = tmp_path / "campaign"; campaign_root.mkdir()
    store = tmp_path / "objects"; store.mkdir()
    registry, registry_hash = _registry(tmp_path)

    def mutate(phase, path):
        if phase == "after_rename":
            path.chmod(0o700)
            target = path / "latency_samples.jsonl"
            target.chmod(0o600)
            target.write_bytes(target.read_bytes() + b"{}\n")

    monkeypatch.setattr(qualification, "_publication_checkpoint", mutate)
    destination = tmp_path / "qualification"
    with pytest.raises(ValueError, match="snapshot changed"):
        qualification.build_metric_qualification(
            campaign_root, object_store=store,
            **_qualification_args(registry, registry_hash), destination=destination)
    assert not destination.exists()


def _real_campaign_fixture(tmp_path):
    repository = Path(__file__).parents[3]
    evidence = (repository / ".opencode/comm-trace-on/managed_exact_candidate_runs")
    source_campaign = evidence / "finalized_formal_campaign"
    source_store = evidence / "campaign_objects"
    if not source_campaign.is_dir() or not source_store.is_dir():
        pytest.fail("real v8/v9 campaign fixture is unavailable")
    campaign_root = tmp_path / "campaign"
    shutil.copytree(source_campaign, campaign_root)
    manifest = json.loads((campaign_root / "campaign_manifest.json").read_bytes())
    descriptors = [item["descriptor"] for item in manifest["registrations"]]
    descriptors.append(manifest["retained_v8_source_descriptor"])
    store = tmp_path / "objects"
    for descriptor in descriptors:
        digest = descriptor["object_sha256"]
        source = source_store / "sha256" / digest[:2] / f"{digest}.tar"
        target = store / "sha256" / digest[:2] / f"{digest}.tar"
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(source, target)
        except OSError:
            shutil.copyfile(source, target)
    seal = json.loads((campaign_root / "campaign_seal.json").read_bytes())
    registry, registry_hash = _registry(
        tmp_path, root=seal["campaign_root_sha256"],
        campaign_id=manifest["campaign_id"])
    return campaign_root, store, registry, registry_hash, manifest["campaign_id"]


def test_installed_cli_builds_and_validates_real_v8_v9_campaign(tmp_path):
    campaign_root, store, registry, registry_hash, campaign_id = (
        _real_campaign_fixture(tmp_path))
    repository = Path(__file__).parents[3]
    cmake_source = tmp_path / "install-project"; cmake_source.mkdir()
    (cmake_source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(ibmw LANGUAGES NONE)\n"
        "include(GNUInstallDirs)\n"
        "set(IBMW_INSTALL ON)\n"
        f"add_subdirectory(\"{repository / 'tools/mw_perf_collect'}\" collector)\n")
    build = tmp_path / "cmake-build"; prefix = tmp_path / "prefix"
    subprocess.run(["cmake", "-S", str(cmake_source), "-B", str(build)], check=True)
    subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix),
                    "--component", "mw_perf_collect"], check=True)
    executable = prefix / "bin/mw-perf-collect"
    installed_parent = prefix / "libexec/ibmw"
    imported = subprocess.run(
        [sys.executable, "-c",
         "import mw_perf_collect,sys;print(mw_perf_collect.__file__)"],
        env={**os.environ, "PYTHONPATH": str(installed_parent)},
        check=True, text=True, capture_output=True).stdout.strip()
    assert imported.startswith(str(prefix))
    destination = tmp_path / "installed-qualification"
    common = ["--campaign", str(campaign_root), "--object-store", str(store),
              "--registry", str(registry), "--registry-sha256", registry_hash,
              "--provider", PROVIDER, "--campaign-id", campaign_id]
    subprocess.run([str(executable), "metric-qualification-build", *common,
                    "--destination", str(destination)], check=True,
                   stdout=subprocess.PIPE, text=True)
    subprocess.run([str(executable), "metric-qualification-validate", *common,
                    "--qualification", str(destination)], check=True,
                   stdout=subprocess.PIPE, text=True)
    invalid_cases = [
        common[:7] + ["0" * 64] + common[8:],
        common[:-3] + ["wrong-provider", "--campaign-id", campaign_id],
        common[:-1] + ["wrong-campaign"],
    ]
    wrong_registry, wrong_hash = _registry(
        tmp_path / "wrong-root", root="0" * 64, campaign_id=campaign_id)
    invalid_cases.append([
        "--campaign", str(campaign_root), "--object-store", str(store),
        "--registry", str(wrong_registry), "--registry-sha256", wrong_hash,
        "--provider", PROVIDER, "--campaign-id", campaign_id])
    for invalid_common in invalid_cases:
        checked = subprocess.run(
            [str(executable), "metric-qualification-validate", *invalid_common,
             "--qualification", str(destination)], stdout=subprocess.PIPE,
            text=True)
        assert checked.returncode != 0
    value = json.loads((destination / "metric_qualification.json").read_bytes())
    assert value["classification"] == {"metric_availability": "supported",
        "valid_for_metric": True, "valid_for_comparison": False}
