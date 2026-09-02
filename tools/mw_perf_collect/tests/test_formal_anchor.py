import hashlib
import json
from pathlib import Path

import pytest

from mw_perf_collect import core
from mw_perf_collect import campaign
from mw_perf_collect import formal_anchor


H = "a" * 64


def _write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(core._canonical_json_bytes(value))


def _object_path(store, descriptor):
    digest = descriptor["object_sha256"]
    return store / "sha256" / digest[:2] / f"{digest}.tar"


def _metadata(role, *, run_id="run-1", contract_id="mw_perf.p0a_contract.v8",
              contract_hash=H, root_hash=H, evidence_hash=H):
    return {"campaign_id": "campaign-1", "role": role,
            "companion_id": f"{role}-1", "run_id": run_id,
            "evidence_contract": {"id": contract_id, "sha256": contract_hash},
            "root_manifest_sha256": root_hash,
            "evidence_manifest_sha256": evidence_hash,
            "build_identity_sha256": "b" * 64}


def _stage1_v8_payload(root):
    contract, contract_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    declarations = core._p0a_declarations(
        contract, "exact_run", False, "exact_correlated", "managed")
    for item in declarations:
        relative = item["relative_path"]
        if (relative.endswith("/") or item["status"] == "not_applicable"
                or relative in {"validation/fault_injection.json", "validation/rollback.json"}):
            continue
        value = ({"schema": item["schema_version"]}
                 if item["format"] == "json" else
                 {"schema": item["schema_version"], "fixture": True})
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(core._canonical_json_bytes(value))
    _write_json(root / "validation/trace_quality.json", {
        "schema": "mw_perf.trace_quality.v1", "fixture": True})
    _write_json(root / "manifest.json", {
        "run_id": "run-1", "candidate_only": True,
        "metric_availability": "unsupported", "valid_for_comparison": False})
    _write_json(root / "validation.json", {
        "validation_profile": "p0a", "valid_for_collection": True,
        "valid_for_metric": False, "valid_for_comparison": False,
        "rules": [{"name": name, "passed": name not in {
            "candidate_only", "metric_available", "p0a_evidence"}}
                  for name in formal_anchor._KNOWN_VALIDATION_RULES],
        "failure_codes": ["CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"],
        "p0a_evidence": {"valid": False, "failure_codes": ["P0A_ARTIFACT"]}})
    _write_json(root / "registration/resolution_declarations.json", {"run_id": "run-1"})
    _write_json(root / "capability/resolution.json", {
        "schema": "mw_perf.p0a.resolution.v2", "run_id": "run-1"})
    evidence = core.build_p0a_evidence_manifest(
        root, {"registration_contract_version": "mw_perf.p0a_contract.v8"},
        run_id="run-1", capability_id="capability-1", path_id="path-1",
        deployment_mode="managed", run_type="exact_run", observer=False,
        measurement_kind="exact_correlated")
    assert evidence is not None
    _write_json(root / "capability/p0a_evidence_manifest.json", evidence)
    _write_json(root / "environment/host.json", {"fixture": True})
    (root / "logs").mkdir(parents=True, exist_ok=True)
    (root / "logs/sut.stderr.log").write_bytes(b"stderr\n")
    (root / "logs/sut.stdout.log").write_bytes(b"stdout\n")
    (root / "raw").mkdir(parents=True, exist_ok=True)
    (root / "raw/network.jsonl").write_bytes(b"\n")
    (root / "raw/process.jsonl").write_bytes(b"\n")
    (root / "report.md").write_bytes(b"# Report\n")
    return contract_raw


def _materialized(tmp_path, monkeypatch):
    payload, store = tmp_path / "stage1", tmp_path / "objects"
    contract_raw = _stage1_v8_payload(payload)
    root_raw = (payload / "manifest.json").read_bytes()
    evidence_raw = (payload / "capability/p0a_evidence_manifest.json").read_bytes()
    source = campaign.build_companion_object_from_snapshot(
        campaign.capture_payload_snapshot(payload), store,
        _metadata("exact_anchor", contract_hash=hashlib.sha256(contract_raw).hexdigest(),
                  root_hash=hashlib.sha256(root_raw).hexdigest(),
                  evidence_hash=hashlib.sha256(evidence_raw).hexdigest()))
    companions = {}
    for role in formal_anchor._ROLES:
        companion_payload = tmp_path / role
        _write_json(companion_payload / "binding.json", {"role": role})
        companions[role] = campaign.build_companion_object_from_snapshot(
            campaign.capture_payload_snapshot(companion_payload), store, _metadata(role))

    anchor_result = {"valid": True, "failure_codes": [], "descriptor": source,
        "run_id": source["run_id"], "root_manifest_sha256": source["root_manifest_sha256"],
        "evidence_manifest_sha256": source["evidence_manifest_sha256"],
        "evidence_contract": source["evidence_contract"],
        "build_identity_sha256": source["build_identity_sha256"]}
    monkeypatch.setattr(campaign, "validate_exact_anchor_companion",
                        lambda path, descriptor: anchor_result)

    def specialized(role, path, descriptor):
        result = {"valid": True, "failure_codes": [], "descriptor": descriptor,
                  "anchor": {key: anchor_result[key] for key in (
                      "run_id", "root_manifest_sha256", "evidence_manifest_sha256",
                      "evidence_contract", "build_identity_sha256")}}
        if role == "fault_runtime_matrix":
            result["production_build_identity_sha256"] = source["build_identity_sha256"]
        return result

    monkeypatch.setattr(campaign, "_specialized_companion", specialized)
    stage_object = _object_path(store, source)
    stage_object_raw = stage_object.read_bytes()
    destination = tmp_path / "formal"
    result = formal_anchor.materialize_formal_anchor(
        stage1_descriptor=source, stage1_object_path=stage_object,
        companion_descriptors=companions, object_store=store,
        destination=destination, campaign_id="campaign-1", companion_id="formal-1")
    return destination, store, source, companions, result, stage_object, stage_object_raw


def test_v9_overrides_v8_paths_without_formal_duplicates():
    v8, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    v9, _ = core._trusted_p0a_contract("mw_perf.p0a_contract.v9")
    old = {item["relative_path"] for item in v8["artifacts"]}
    new = {item["relative_path"] for item in v9["artifacts"]}
    assert new == old | {"validation/formal_anchor.json"}
    assert not any(path.endswith("_formal.json") for path in new)
    assert v9["run_types"]["formal_p0a"]["universes"] == [
        "proof", "collection", "observer", "formal_anchor"]


def test_formal_schema_documents_are_closed():
    schema_dir = Path(formal_anchor.__file__).parent / "schemas"
    for name in ("formal-anchor.schema.v1.json", "fault-injection-formal.schema.v2.json",
                 "rollback-formal.schema.v2.json", "trace-quality-formal.schema.v2.json"):
        schema = json.loads((schema_dir / name).read_bytes())
        assert not core._schema_errors(schema, {"type": "object"})
        assert schema["additionalProperties"] is False
        assert all(item.get("additionalProperties") is False
                   for item in schema.get("$defs", {}).values()
                   if isinstance(item, dict) and item.get("type") == "object")


def test_materialize_formal_anchor_happy_path(tmp_path, monkeypatch):
    (destination, store, source, companions, result,
     stage_object, stage_object_raw) = _materialized(tmp_path, monkeypatch)

    assert stage_object.read_bytes() == stage_object_raw
    assert result["source_descriptor"] == source
    packaged = result["formal_descriptor"]
    assert packaged["role"] == "exact_anchor"
    assert packaged["evidence_contract"]["id"] == "mw_perf.p0a_contract.v9"
    assert _object_path(store, packaged).is_file()
    assert (destination / "validation/formal_anchor.json").is_file()

    formal = json.loads((destination / "validation/formal_anchor.json").read_bytes())
    fault = json.loads((destination / "validation/fault_injection.json").read_bytes())
    rollback = json.loads((destination / "validation/rollback.json").read_bytes())
    trace = json.loads((destination / "validation/trace_quality.json").read_bytes())
    evidence = json.loads((destination / "capability/p0a_evidence_manifest.json").read_bytes())
    validation = json.loads((destination / "validation.json").read_bytes())
    assert formal["source_anchor"] == source
    assert formal["companions"] == companions
    assert fault["companions"] == {
        role: companions[role]
        for role in ("fault_evidence_tamper", "fault_runtime_matrix")}
    assert rollback["companion"] == companions["rollback_trace_off"]
    assert "companion" not in trace and "companions" not in trace
    stage_trace = campaign._read_canonical_payload_object(stage_object)[
        "validation/trace_quality.json"]
    assert trace["source_trace_quality_content_sha256"] == hashlib.sha256(stage_trace).hexdigest()
    assert trace["source_trace_quality_content_sha256"] == formal[
        "source_trace_quality_content_sha256"]
    assert evidence["registration_contract_version"] == "mw_perf.p0a_contract.v9"
    assert evidence["runtime_changed"] is True
    assert validation["valid_for_p0a_completion"] is True
    assert validation["failure_codes"] == ["METRIC_AVAILABLE"]
    assert validation["p0a_evidence"] == {"valid": True, "failure_codes": []}
    assert [item["name"] for item in validation["rules"]] == list(
        formal_anchor._KNOWN_VALIDATION_RULES)
    assert validation["rules"][0] == {"name": "resource_integrity", "passed": True}
    states = {item["name"]: item["passed"] for item in validation["rules"]}
    assert states["candidate_only"] is True
    assert states["metric_available"] is False
    assert states["p0a_evidence"] is True
    assert all(value is True for name, value in states.items()
               if name not in {"candidate_only", "metric_available", "p0a_evidence"})
    assert formal_anchor.validate_formal_anchor_snapshot(destination)["valid"]


@pytest.mark.parametrize(("relative", "mutate"), [
    ("validation/fault_injection.json",
     lambda value: value["companions"]["fault_evidence_tamper"].update(
         companion_id="different")),
    ("validation/fault_injection.json",
     lambda value: value["companions"]["fault_runtime_matrix"].update(
         companion_id="different")),
    ("validation/rollback.json",
     lambda value: value["companion"].update(companion_id="different")),
    ("validation/trace_quality.json",
     lambda value: value.update(source_trace_quality_content_sha256="0" * 64)),
])
def test_snapshot_validator_rejects_each_formal_document_binding(
        tmp_path, monkeypatch, relative, mutate):
    destination, *_ = _materialized(tmp_path, monkeypatch)
    target = destination / relative
    value = json.loads(target.read_bytes())
    mutate(value)
    target.write_bytes(core._canonical_json_bytes(value))
    evidence_path = destination / "capability/p0a_evidence_manifest.json"
    evidence = json.loads(evidence_path.read_bytes())
    entry = next(item for item in evidence["entries"] if item["relative_path"] == relative)
    entry["sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
    evidence["required"] = [item for item in evidence["entries"]
                            if item["status"] == "required"]
    evidence_path.write_bytes(core._canonical_json_bytes(evidence))
    with pytest.raises(ValueError):
        formal_anchor.validate_formal_anchor_snapshot(destination)


def test_formal_schemas_reject_descriptor_role_tampering():
    schema_dir = Path(formal_anchor.__file__).parent / "schemas"
    descriptor = _metadata("exact_anchor") | {
        "object_sha256": H, "bundle_manifest_sha256": H, "archive_tree_sha256": H}
    cases = {
        "fault-injection-formal.schema.v2.json": {
            "schema": "mw_perf.fault_injection_formal.v2",
            "companions": {"fault_evidence_tamper": dict(descriptor, role="exact_anchor"),
                           "fault_runtime_matrix": dict(descriptor, role="fault_runtime_matrix")}},
        "rollback-formal.schema.v2.json": {
            "schema": "mw_perf.rollback_formal.v2",
            "companion": dict(descriptor, role="exact_anchor")},
    }
    base = {"run_id": "run-1", "formal_anchor_sha256": H,
            "source_anchor": descriptor, "source_evidence_manifest_sha256": H,
            "finalized": True, "status": {"valid_for_p0a_completion": True,
            "metric_availability": "unsupported", "valid_for_comparison": False},
            "conclusion": "pass"}
    for name, fields in cases.items():
        schema = json.loads((schema_dir / name).read_bytes())
        assert core._schema_errors(base | fields, schema)

    formal_schema = json.loads((schema_dir / "formal-anchor.schema.v1.json").read_bytes())
    formal = {"schema": "mw_perf.formal_anchor.v1", "formal_anchor_id": H,
              "run_id": "run-1", "registration_contract": {
                  "version": "mw_perf.p0a_contract.v9", "sha256": H},
              "source_anchor": descriptor,
              "companions": {
                  "fault_evidence_tamper": dict(descriptor, role="fault_runtime_matrix"),
                  "fault_runtime_matrix": dict(descriptor, role="fault_runtime_matrix"),
                  "rollback_trace_off": dict(descriptor, role="rollback_trace_off")},
              "source_trace_quality_content_sha256": H, "finalized": True,
              "status": {"valid_for_p0a_completion": True,
                         "metric_availability": "unsupported",
                         "valid_for_comparison": False}, "conclusion": "pass"}
    assert core._schema_errors(formal, formal_schema)
    formal["companions"]["fault_evidence_tamper"]["role"] = "fault_evidence_tamper"
    formal["source_anchor"]["role"] = "fault_runtime_matrix"
    assert core._schema_errors(formal, formal_schema)


def test_snapshot_validator_rejects_stale_formal_hash(tmp_path):
    (tmp_path / "validation").mkdir()
    (tmp_path / "capability").mkdir()
    (tmp_path / "validation/formal_anchor.json").write_text(json.dumps({
        "schema": "mw_perf.formal_anchor.v1", "formal_anchor_id": "0" * 64,
        "run_id": "run", "registration_contract": {"version": "mw_perf.p0a_contract.v9", "sha256": "0" * 64},
        "source_anchor": {}, "companions": {}, "source_trace_quality_content_sha256": "0" * 64,
        "finalized": True, "status": {"valid_for_p0a_completion": True,
        "metric_availability": "unsupported", "valid_for_comparison": False}, "conclusion": "pass"}))
    with pytest.raises(ValueError):
        formal_anchor.validate_formal_anchor_snapshot(tmp_path)


def _archive_context(tmp_path, monkeypatch):
    _destination, store, source, companions, result, *_ = _materialized(
        tmp_path, monkeypatch)
    descriptor = result["formal_descriptor"]
    payloads = campaign._read_canonical_payload_object(_object_path(store, descriptor))
    return payloads, descriptor, source, companions, store


def _replace_json(payloads, relative, mutate, *, rebind_evidence=True):
    value = json.loads(payloads[relative])
    mutate(value)
    payloads[relative] = core._canonical_json_bytes(value, newline=True)
    if rebind_evidence and relative != formal_anchor._EVIDENCE:
        evidence = json.loads(payloads[formal_anchor._EVIDENCE])
        entry = next(item for item in evidence["entries"]
                     if item["relative_path"] == relative)
        entry["sha256"] = hashlib.sha256(payloads[relative]).hexdigest()
        evidence["required"] = [item for item in evidence["entries"]
                                if item["status"] == "required"]
        evidence["conditional"] = [item for item in evidence["entries"]
                                   if item["status"] != "required"]
        payloads[formal_anchor._EVIDENCE] = core._canonical_json_bytes(
            evidence, newline=True)


def _validate_archive(payloads, descriptor, source, companions):
    return formal_anchor.validate_formal_archive_payload(
        payloads, formal_descriptor=descriptor, source_descriptor=source,
        companion_descriptors=companions)


@pytest.mark.parametrize(("relative", "mutate"), [
    ("validation/fault_injection.json",
     lambda value: value["companions"].pop("fault_runtime_matrix")),
    ("validation/rollback.json",
     lambda value: value["companion"].update(companion_id="wrong")),
    ("validation/trace_quality.json",
     lambda value: value.update(source_trace_quality_content_sha256="0" * 64)),
    ("validation/fault_injection.json",
     lambda value: value.update(source_evidence_manifest_sha256="0" * 64)),
    ("validation/rollback.json", lambda value: value["status"].update(
        valid_for_p0a_completion=False)),
    ("validation/trace_quality.json", lambda value: value.update(conclusion="fail")),
])
def test_archive_validator_rejects_each_false_formal_binding(
        tmp_path, monkeypatch, relative, mutate):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    _replace_json(payloads, relative, mutate)
    with pytest.raises(ValueError):
        _validate_archive(payloads, descriptor, source, companions)


def test_archive_validator_rejects_extra_formal_failure(tmp_path, monkeypatch):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    _replace_json(payloads, "validation.json",
                  lambda value: value["failure_codes"].append("EXTRA_FORMAL_FAILURE"),
                  rebind_evidence=False)
    with pytest.raises(ValueError):
        _validate_archive(payloads, descriptor, source, companions)


@pytest.mark.parametrize(("field", "value"), [
    ("root_manifest_sha256", "0" * 64),
    ("evidence_manifest_sha256", "0" * 64),
    ("build_identity_sha256", "0" * 64),
    ("run_id", "other-run"),
    ("campaign_id", "other-campaign"),
    ("role", "fault_runtime_matrix"),
])
def test_archive_validator_rejects_false_outer_descriptor_metadata(
        tmp_path, monkeypatch, field, value):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    changed = dict(descriptor)
    changed[field] = value
    with pytest.raises(ValueError):
        _validate_archive(payloads, changed, source, companions)


def test_archive_validator_rejects_false_outer_contract_metadata(tmp_path, monkeypatch):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    changed = dict(descriptor)
    changed["evidence_contract"] = {"id": "mw_perf.p0a_contract.v9", "sha256": "0" * 64}
    with pytest.raises(ValueError):
        _validate_archive(payloads, changed, source, companions)


def test_archive_validator_rejects_untrusted_v9_contract_and_incomplete_manifest(
        tmp_path, monkeypatch):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    fake = core._canonical_json_bytes({
        "registration_contract_version": "mw_perf.p0a_contract.v9",
        "artifacts": [], "artifact_schemas": {}}, newline=True)
    payloads[formal_anchor._CONTRACT] = fake
    evidence = json.loads(payloads[formal_anchor._EVIDENCE])
    evidence["registration_contract_sha256"] = hashlib.sha256(fake).hexdigest()
    payloads[formal_anchor._EVIDENCE] = core._canonical_json_bytes(evidence, newline=True)
    with pytest.raises(ValueError):
        _validate_archive(payloads, descriptor, source, companions)

    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path / "second", monkeypatch)
    evidence = json.loads(payloads[formal_anchor._EVIDENCE])
    evidence["entries"].pop()
    evidence["required"] = [item for item in evidence["entries"]
                            if item["status"] == "required"]
    evidence["conditional"] = [item for item in evidence["entries"]
                               if item["status"] != "required"]
    payloads[formal_anchor._EVIDENCE] = core._canonical_json_bytes(evidence, newline=True)
    with pytest.raises(ValueError):
        _validate_archive(payloads, descriptor, source, companions)


def test_archive_validator_rejects_nested_campaign_seal(tmp_path, monkeypatch):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    payloads["raw/comm_trace/nested/campaign_seal.json"] = b"{}\n"
    with pytest.raises(ValueError):
        _validate_archive(payloads, descriptor, source, companions)

    destination, *_ = _materialized(tmp_path / "snapshot", monkeypatch)
    seal = destination / "raw/comm_trace/nested/campaign_seal.json"
    seal.parent.mkdir(parents=True, exist_ok=True)
    seal.write_text("{}\n")
    with pytest.raises(ValueError):
        formal_anchor.validate_formal_anchor_snapshot(destination)


def test_archive_validator_allows_inherited_logs_and_report_only(tmp_path, monkeypatch):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    payloads.update({
        "logs/sut.stdout.log": b"stdout\n",
        "logs/sut.stderr.log": b"stderr\n",
        "report.md": b"# Report\n",
    })
    assert _validate_archive(payloads, descriptor, source, companions)["valid"]

    for relative in (
            "arbitrary.txt",
            "unknown/artifact.json",
            "logs/nested/final-seal.json",
            "report-final_seal.json"):
        changed = dict(payloads)
        changed[relative] = b"{}\n"
        with pytest.raises(ValueError):
            _validate_archive(changed, descriptor, source, companions)


@pytest.mark.parametrize("relative", [
    "registration/unbound.json", "capability/unbound.json",
    "validation/unbound.json", "raw/unbound.json", "environment/unbound.json",
    "normalized/unbound.json", "logs/unbound.log",
])
def test_archive_validator_rejects_unbound_file_under_each_root(
        tmp_path, monkeypatch, relative):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    changed = dict(payloads)
    changed[relative] = b"{}\n"
    with pytest.raises(ValueError):
        _validate_archive(changed, descriptor, source, companions)


def test_archive_validator_rejects_unexpected_top_level_and_not_applicable_file(
        tmp_path, monkeypatch):
    payloads, descriptor, source, companions, _store = _archive_context(
        tmp_path, monkeypatch)
    for relative in ("unexpected.txt", "capability/observer_isolation.json"):
        changed = dict(payloads)
        changed[relative] = b"{}\n"
        with pytest.raises(ValueError):
            _validate_archive(changed, descriptor, source, companions)


def test_snapshot_validator_rejects_directory_symlink(tmp_path, monkeypatch):
    destination, *_ = _materialized(tmp_path, monkeypatch)
    (destination / "linked-directory").symlink_to(destination / "logs",
                                                   target_is_directory=True)
    with pytest.raises(ValueError):
        formal_anchor.validate_formal_anchor_snapshot(destination)


@pytest.mark.parametrize("invalid", [
    b'{"validation_profile":"p0a","validation_profile":"p0a"}\n',
    b'{"validation_profile":"p0a","strict_probe":NaN}\n',
    b'{"validation_profile":"p0a","strict_probe":Infinity}\n',
])
def test_formal_snapshot_strictly_rejects_duplicate_and_nonfinite_json(
        tmp_path, monkeypatch, invalid):
    destination, *_ = _materialized(tmp_path, monkeypatch)
    target = destination / "validation/fault_injection.json"
    target.write_bytes(invalid)
    evidence_path = destination / formal_anchor._EVIDENCE
    evidence = json.loads(evidence_path.read_bytes())
    entry = next(item for item in evidence["entries"]
                 if item["relative_path"] == "validation/fault_injection.json")
    entry["sha256"] = hashlib.sha256(invalid).hexdigest()
    evidence["required"] = [item for item in evidence["entries"]
                            if item["status"] == "required"]
    evidence_path.write_bytes(core._canonical_json_bytes(evidence, newline=True))
    with pytest.raises(ValueError, match="invalid formal anchor snapshot"):
        formal_anchor.validate_formal_anchor_snapshot(destination)


def test_formal_validation_transform_rejects_unrelated_failure_and_is_exact():
    source = {"validation_profile": "p0a", "valid_for_collection": True,
              "valid_for_metric": False, "valid_for_comparison": False,
              "failure_codes": ["CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"],
              "rules": [{"name": name, "passed": name not in {
                  "candidate_only", "metric_available", "p0a_evidence"}}
                        for name in formal_anchor._KNOWN_VALIDATION_RULES],
              "p0a_evidence": {"valid": False,
                               "failure_codes": ["P0A_ARTIFACT"]}}
    formal = formal_anchor._formalize_validation(source)
    formal_anchor._validate_formal_validation(formal)
    states = {item["name"]: item["passed"] for item in formal["rules"]}
    assert states["candidate_only"] is True
    assert states["p0a_evidence"] is True
    assert states["metric_available"] is False
    assert all(value is True for name, value in states.items()
               if name not in {"candidate_only", "p0a_evidence", "metric_available"})
    for mutate in (
            lambda value: value["failure_codes"].append("UNRELATED"),
            lambda value: value["failure_codes"].remove("METRIC_AVAILABLE"),
            lambda value: next(item for item in value["rules"]
                               if item["name"] == "metric_available").update(passed=True),
            lambda value: next(item for item in value["rules"]
                               if item["name"] == "candidate_only").update(passed=True),
            lambda value: next(item for item in value["rules"]
                               if item["name"] == "p0a_evidence").update(passed=True)):
        changed = json.loads(json.dumps(source))
        mutate(changed)
        with pytest.raises(ValueError):
            formal_anchor._formalize_validation(changed)

    formal["failure_codes"].append("EXTRA_FORMAL_FAILURE")
    with pytest.raises(ValueError):
        formal_anchor._validate_formal_validation(formal)


@pytest.mark.parametrize("mutate", [
    lambda rules: rules.pop(0),
    lambda rules: rules.__setitem__(slice(0, 2), reversed(rules[:2])),
    lambda rules: rules[0].update(passed=False),
    lambda rules: rules.insert(0, {"name": "unknown_rule", "passed": True}),
])
def test_formal_validation_rejects_missing_reordered_failed_or_unknown_rules(mutate):
    source = {"validation_profile": "p0a", "valid_for_collection": True,
              "valid_for_metric": False, "valid_for_comparison": False,
              "failure_codes": ["CANDIDATE_ONLY", "METRIC_AVAILABLE", "P0A_EVIDENCE"],
              "rules": [{"name": name, "passed": name not in {
                  "candidate_only", "metric_available", "p0a_evidence"}}
                        for name in formal_anchor._KNOWN_VALIDATION_RULES],
              "p0a_evidence": {"valid": False,
                               "failure_codes": ["P0A_ARTIFACT"]}}
    formal = formal_anchor._formalize_validation(source)
    changed_source = json.loads(json.dumps(source))
    changed_formal = json.loads(json.dumps(formal))
    mutate(changed_source["rules"])
    mutate(changed_formal["rules"])

    with pytest.raises(ValueError):
        formal_anchor._formalize_validation(changed_source)
    with pytest.raises(ValueError):
        formal_anchor._validate_formal_validation(changed_formal)


def test_v8_exact_archive_allows_only_immutable_companion_placeholders(tmp_path):
    payload, store = tmp_path / "source", tmp_path / "objects"
    contract_raw = _stage1_v8_payload(payload)
    descriptor = campaign.build_companion_object_from_snapshot(
        campaign.capture_payload_snapshot(payload), store,
        _metadata("exact_anchor", contract_hash=hashlib.sha256(contract_raw).hexdigest(),
                  root_hash=hashlib.sha256((payload / "manifest.json").read_bytes()).hexdigest(),
                  evidence_hash=hashlib.sha256(
                      (payload / formal_anchor._EVIDENCE).read_bytes()).hexdigest()))
    assert campaign.validate_exact_anchor_companion(
        _object_path(store, descriptor), descriptor)["valid"]

    files = dict(campaign.capture_payload_snapshot(payload).files)
    evidence = json.loads(files[formal_anchor._EVIDENCE])
    evidence["entries"].pop()
    evidence["required"] = [item for item in evidence["entries"]
                            if item["status"] == "required"]
    files[formal_anchor._EVIDENCE] = core._canonical_json_bytes(evidence, newline=True)
    incomplete = campaign.build_companion_object_from_snapshot(
        campaign.PayloadSnapshot(files), store,
        _metadata("exact_anchor",
                  contract_hash=hashlib.sha256(contract_raw).hexdigest(),
                  root_hash=descriptor["root_manifest_sha256"],
                  evidence_hash=hashlib.sha256(files[formal_anchor._EVIDENCE]).hexdigest())
        | {"companion_id": "incomplete"})
    assert not campaign.validate_exact_anchor_companion(
        _object_path(store, incomplete), incomplete)["valid"]

    files = dict(campaign.capture_payload_snapshot(payload).files)
    fake_contract = core._canonical_json_bytes({
        "registration_contract_version": "mw_perf.p0a_contract.v8",
        "artifacts": [], "artifact_schemas": {}}, newline=True)
    files[formal_anchor._CONTRACT] = fake_contract
    evidence = json.loads(files[formal_anchor._EVIDENCE])
    evidence["registration_contract_sha256"] = hashlib.sha256(fake_contract).hexdigest()
    files[formal_anchor._EVIDENCE] = core._canonical_json_bytes(evidence, newline=True)
    untrusted = campaign.build_companion_object_from_snapshot(
        campaign.PayloadSnapshot(files), store,
        _metadata("exact_anchor",
                  contract_hash=hashlib.sha256(fake_contract).hexdigest(),
                  root_hash=descriptor["root_manifest_sha256"],
                  evidence_hash=hashlib.sha256(files[formal_anchor._EVIDENCE]).hexdigest())
        | {"companion_id": "untrusted"})
    assert not campaign.validate_exact_anchor_companion(
        _object_path(store, untrusted), untrusted)["valid"]


def test_trusted_evidence_rejects_invalid_v8_placeholders_and_v9_incomplete_entry(
        tmp_path, monkeypatch):
    payload = tmp_path / "stage1"
    _stage1_v8_payload(payload)
    payloads = dict(campaign.capture_payload_snapshot(payload).files)
    evidence_path = formal_anchor._EVIDENCE
    allowed = {"validation/fault_injection.json", "validation/rollback.json"}

    def validate_v8(files, value):
        campaign._validate_trusted_evidence_payload(
            files, value, expected_version="mw_perf.p0a_contract.v8",
            expected_run_type="exact_run", expected_run_id="run-1",
            allowed_incomplete_paths=allowed)

    validate_v8(payloads, json.loads(payloads[evidence_path]))
    for relative, mutate in (
            ("validation/fault_injection.json",
             lambda item: item.update(finalization="complete")),
            ("validation/rollback.json",
             lambda item: item.update(sha256="0" * 64)),
            ("validation/rollback.json",
             lambda item: item.update(record_count=1)),):
        changed = dict(payloads)
        value = json.loads(changed[evidence_path])
        entry = next(item for item in value["entries"] if item["relative_path"] == relative)
        mutate(entry)
        changed[evidence_path] = core._canonical_json_bytes(value, newline=True)
        with pytest.raises(ValueError):
            validate_v8(changed, value)

    present = dict(payloads)
    present["validation/fault_injection.json"] = b"{}\n"
    with pytest.raises(ValueError):
        validate_v8(present, json.loads(present[evidence_path]))

    missing = dict(payloads)
    value = json.loads(missing[evidence_path])
    value["entries"] = [item for item in value["entries"]
                         if item["relative_path"] != "validation/rollback.json"]
    value["required"] = [item for item in value["entries"] if item["status"] == "required"]
    value["conditional"] = [item for item in value["entries"] if item["status"] != "required"]
    missing[evidence_path] = core._canonical_json_bytes(value, newline=True)
    with pytest.raises(ValueError):
        validate_v8(missing, value)

    formal_payloads, formal_descriptor, source, companions, _store = _archive_context(
        tmp_path / "v9", monkeypatch)
    formal_evidence = json.loads(formal_payloads[evidence_path])
    campaign._validate_trusted_evidence_payload(
        formal_payloads, formal_evidence, expected_version="mw_perf.p0a_contract.v9",
        expected_run_type="formal_p0a", expected_run_id=formal_descriptor["run_id"],
        allowed_incomplete_paths=frozenset())
    changed = dict(formal_payloads)
    changed_evidence = json.loads(changed[evidence_path])
    changed_evidence["entries"][0].update(finalization="incomplete", record_count=0,
                                           sha256=None)
    changed[evidence_path] = core._canonical_json_bytes(changed_evidence, newline=True)
    with pytest.raises(ValueError):
        campaign._validate_trusted_evidence_payload(
            changed, changed_evidence, expected_version="mw_perf.p0a_contract.v9",
            expected_run_type="formal_p0a", expected_run_id=formal_descriptor["run_id"],
            allowed_incomplete_paths=frozenset())


def test_formal_object_default_store_and_exact_expected_closure(tmp_path, monkeypatch):
    payloads, descriptor, source, companions, store = _archive_context(
        tmp_path, monkeypatch)
    del payloads
    path = _object_path(store, descriptor)
    assert formal_anchor.validate_formal_exact_anchor_companion(
        path, descriptor, expected_source_descriptor=source,
        expected_companion_descriptors=companions)["valid"]
    changed = dict(source, companion_id="other-source")
    assert not formal_anchor.validate_formal_exact_anchor_companion(
        path, descriptor, expected_source_descriptor=changed,
        expected_companion_descriptors=companions)["valid"]


@pytest.mark.parametrize("role", ["exact_anchor", *formal_anchor._ROLES])
def test_formal_campaign_rejects_valid_public_object_outside_embedded_closure(
        tmp_path, monkeypatch, role):
    _payloads, formal_descriptor, source, companions, store = _archive_context(
        tmp_path, monkeypatch)
    registrations = {"exact_anchor": formal_descriptor, **companions}
    retained = source
    original = source if role == "exact_anchor" else companions[role]
    original_payloads = campaign._read_canonical_payload_object(
        _object_path(store, original))
    metadata = {key: original[key] for key in campaign._METADATA_KEYS}
    metadata["companion_id"] = f"public-{role}"
    alternate = campaign.build_companion_object_from_snapshot(
        campaign.PayloadSnapshot(original_payloads), store, metadata)
    if role == "exact_anchor":
        retained = alternate
    else:
        registrations[role] = alternate
    with pytest.raises(ValueError, match="formal exact anchor validation failed"):
        campaign.finalize_formal_campaign(
            tmp_path / "campaign", campaign_id="campaign-1",
            descriptors=registrations,
            retained_v8_source_descriptor=retained, object_store=store)


def test_formal_campaign_round_trip_uses_external_root(tmp_path, monkeypatch):
    _payloads, formal_descriptor, source, companions, store = _archive_context(
        tmp_path, monkeypatch)
    published = campaign.finalize_formal_campaign(
        tmp_path / "campaign", campaign_id="campaign-1",
        descriptors={"exact_anchor": formal_descriptor, **companions},
        retained_v8_source_descriptor=source, object_store=store)
    assert campaign.validate_campaign(
        tmp_path / "campaign", object_store=store,
        expected_campaign_root_sha256=published["campaign_root_sha256"])["valid"]
    assert not campaign.validate_campaign(
        tmp_path / "campaign", object_store=store,
        expected_campaign_root_sha256="0" * 64)["valid"]
