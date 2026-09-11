"""Content-sealed/read-only qualification of one exact DDS callback latency metric."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import secrets
import stat
from pathlib import Path
from typing import Any, Mapping

from . import campaign, core, dds_trace, formal_anchor
from .json_strict import loads as strict_json_loads


SCHEMA = "mw_perf.metric_qualification.v1"
CAPABILITY = "dds.ordinary.nonplain.single_driver.sync_callback"
_DECLARATION = "metric-qualification-contract.v1.json"
_ID_DOMAIN = b"mw_perf.metric_qualification.id.v1\0"
_ARTIFACT_DOMAIN = b"mw_perf.metric_qualification.artifact.v1\0"
_FILES = frozenset({"declaration.json", "latency_samples.jsonl",
                    "metric_qualification.json", "qualification_seal.json"})
_REGISTRY_SCHEMA = "mw_perf.trusted_external_roots.v1"
_LIMITATIONS = [
    "No percentile claim.",
    "No threshold claim.",
    "No overhead claim.",
    "No ranking claim.",
    "No regression claim.",
    "No performance acceptance claim.",
]


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical(value: Any, *, newline: bool = True) -> bytes:
    return core._canonical_json_bytes(value, newline=newline)


def _trusted_declaration() -> tuple[dict[str, Any], bytes]:
    path = Path(__file__).parent / "contracts" / _DECLARATION
    try:
        raw = path.read_bytes()
        value = strict_json_loads(raw)
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("metric qualification declaration is unavailable") from exc
    if not isinstance(value, dict) or raw != _canonical(value):
        raise ValueError("metric qualification declaration is not canonical")
    return value, raw


def load_trusted_external_root(registry_path: Path, *,
                               expected_registry_sha256: str,
                               provider: str,
                               campaign_id: str) -> tuple[dict[str, Any], bytes]:
    """Load one operator-approved root from an externally hash-pinned registry."""
    try:
        raw = Path(registry_path).read_bytes()
        if _sha(raw) != expected_registry_sha256:
            raise ValueError("trusted external-root registry hash mismatch")
        value = strict_json_loads(raw)
        schema = strict_json_loads((Path(__file__).parent / "schemas" /
                                    "trusted-external-roots.schema.v1.json").read_bytes())
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("trusted external-root registry is unavailable") from exc
    if (not isinstance(value, dict) or not isinstance(schema, dict)
            or raw != _canonical(value) or core._schema_errors(value, schema)):
        raise ValueError("trusted external-root registry is not a canonical closed document")
    entries = value["entries"]
    identities = [(entry["provider"], entry["campaign_id"]) for entry in entries]
    if len(set(identities)) != len(identities):
        raise ValueError("trusted external-root registry identities are not unique")
    matches = [entry for entry in entries
               if entry["provider"] == provider and entry["campaign_id"] == campaign_id]
    if len(matches) != 1:
        raise ValueError("trusted external-root registry entry is unavailable")
    return dict(matches[0]), raw


def _json(payloads: Mapping[str, bytes], relative: str) -> dict[str, Any]:
    raw = payloads[relative]
    value = strict_json_loads(raw)
    if not isinstance(value, dict) or raw != _canonical(value):
        raise ValueError(f"{relative} is not canonical JSON")
    return value


def _historical_v8_json(payloads: Mapping[str, bytes], relative: str) -> dict[str, Any]:
    """Strictly parse a byte-bound v8 artifact whose original format is retained."""
    value = strict_json_loads(payloads[relative])
    if not isinstance(value, dict):
        raise ValueError(f"{relative} is not a valid JSON object")
    return value


def _source_derivation(campaign_root: Path, object_store: Path,
                       expected_campaign_root_sha256: str) -> dict[str, Any]:
    checked = campaign.validate_campaign(
        Path(campaign_root), object_store=Path(object_store),
        expected_campaign_root_sha256=expected_campaign_root_sha256)
    manifest = checked.get("manifest")
    if (checked.get("valid") is not True or not isinstance(manifest, dict)
            or manifest.get("schema") != "mw_perf.evidence_campaign.v2"):
        raise ValueError("qualification requires an externally rooted valid campaign v2")
    registrations = manifest.get("registrations")
    if not isinstance(registrations, list):
        raise ValueError("formal campaign registrations are unavailable")
    formal_descriptor = next(
        (item.get("descriptor") for item in registrations
         if isinstance(item, dict) and item.get("role") == "exact_anchor"), None)
    source_descriptor = manifest.get("retained_v8_source_descriptor")
    if not isinstance(formal_descriptor, dict) or not isinstance(source_descriptor, dict):
        raise ValueError("formal and source descriptors are unavailable")

    formal_path = campaign._checked_object_path(Path(object_store),
                                                formal_descriptor["object_sha256"])
    formal_payloads = campaign._read_canonical_payload_object(formal_path)
    formal = _json(formal_payloads, formal_anchor._FORMAL)
    if formal.get("source_anchor") != source_descriptor:
        raise ValueError("formal archive source closure is inconsistent")

    source_path = campaign._checked_object_path(Path(object_store),
                                                source_descriptor["object_sha256"])
    if campaign.validate_exact_anchor_companion(source_path, source_descriptor).get("valid") is not True:
        raise ValueError("v8 exact source archive is invalid")
    payloads = campaign._read_canonical_payload_object(source_path)
    evidence = _historical_v8_json(payloads, formal_anchor._EVIDENCE)
    contract, contract_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    campaign._validate_trusted_evidence_payload(
        payloads, evidence, expected_version="mw_perf.p0a_contract.v8",
        expected_run_type="exact_run", expected_run_id=source_descriptor["run_id"],
        allowed_incomplete_paths={"validation/fault_injection.json", "validation/rollback.json"})
    if evidence.get("capability_id") != CAPABILITY:
        raise ValueError("source capability is not qualified")

    prefix = "raw/comm_trace/"
    raw_files = {name.removeprefix(prefix): data for name, data in payloads.items()
                 if name.startswith(prefix) and name != prefix + "directory_finalization.json"}
    meta_names = [name for name in raw_files if name.endswith(".meta.json")
                  and not name.endswith(".exact.meta.json")]
    if len(meta_names) != 1:
        raise ValueError("source trace has no unique stage metadata")
    meta = strict_json_loads(raw_files[meta_names[0]])
    if not isinstance(meta, dict):
        raise ValueError("source trace metadata is invalid")
    expected = {field: evidence[field] for field in
                ("run_id", "capability_id", "path_id", "deployment_mode")}
    identity = dds_trace.derive_identity_trace(
        raw_files, contract["artifact_schemas"], core._schema_errors, expected,
        producer_pid=meta["producer_pid"],
        producer_start_time_ticks=meta["producer_start_time_ticks"],
        topology_generation=meta["topology_generation"],
        topology_sha256=meta["topology_sha256"])
    resolution_raw = payloads["capability/resolution.json"]
    resolution = strict_json_loads(resolution_raw)
    if not isinstance(resolution, dict) or resolution_raw != _canonical(resolution):
        raise ValueError("source resolution is not canonical")
    contract_sha = _sha(contract_raw)
    hook = dds_trace.derive_hook_callgraph(
        identity, resolution, resolution_raw,
        registration_contract_sha256=contract_sha)
    normalized = dds_trace.derive_normalized_results(
        identity, resolution, resolution_raw, hook, hook["bytes"])
    latency = dds_trace.derive_communication_latency(
        identity, resolution, resolution_raw, hook, hook["bytes"], normalized,
        registration_contract_sha256=contract_sha)
    delivery = dds_trace.derive_communication_delivery(
        identity, resolution, resolution_raw, hook, hook["bytes"], normalized, latency,
        registration_contract_sha256=contract_sha,
        expectations={"publish_count": 1000, "readers_per_sample": 1,
                      "ordering_policy":
                      "dds_identity_monotonic_stage_and_callback_completion_records"})
    quality = dds_trace.derive_trace_quality(
        identity, delivery, registration_contract_version="mw_perf.p0a_contract.v8",
        registration_contract_sha256=contract_sha)
    exact_bytes = {
        "normalized/identity_trace.jsonl": identity["bytes"],
        "capability/hook_callgraph.json": hook["bytes"],
        "normalized/comm_events.jsonl": normalized["comm_events"]["bytes"],
        "normalized/latency_samples.jsonl": normalized["latency_samples"]["bytes"],
        "capability/communication_latency.json": latency["bytes"],
        "validation/communication_delivery.json": delivery["bytes"],
        "validation/trace_quality.json": quality["bytes"],
    }
    if any(payloads.get(name) != data for name, data in exact_bytes.items()):
        raise ValueError("source derived artifact closure is stale or rewritten")
    quality_schema = strict_json_loads(
        (Path(__file__).parent / "schemas/trace-quality.schema.v1.json").read_bytes())
    if not isinstance(quality_schema, dict) or core._schema_errors(quality["value"], quality_schema):
        raise ValueError("source trace quality does not satisfy the trusted schema")
    return {"manifest": manifest, "formal_descriptor": formal_descriptor,
            "formal": formal, "source_descriptor": source_descriptor,
            "latency": latency, "delivery": delivery, "quality": quality,
            "samples": normalized["latency_samples"], "identity": identity}


def _qualification(source: Mapping[str, Any], declaration_sha256: str,
                   registry_entry: Mapping[str, Any], registry_sha256: str) -> dict[str, Any]:
    latency = source["latency"]; delivery = source["delivery"]
    quality = source["quality"]; samples = source["samples"]
    formal = source["formal"]
    value: dict[str, Any] = {
        "schema": SCHEMA,
        "qualification_id": "0" * 64,
        "capability_id": CAPABILITY,
        "source_campaign": {"provider": registry_entry["provider"],
                            "campaign_id": source["manifest"]["campaign_id"],
                            "external_root_sha256": registry_entry["external_root_sha256"],
                            "registry": {"schema": _REGISTRY_SCHEMA,
                                         "content_sha256": registry_sha256,
                                         "entry": dict(registry_entry)}},
        "formal_anchor": {"descriptor": source["formal_descriptor"],
                          "formal_anchor_id": formal["formal_anchor_id"],
                          "content_sha256": _sha(_canonical(formal))},
        "source_anchor": source["source_descriptor"],
        "declaration": {"id": "mw_perf.metric_qualification_contract.v1",
                        "sha256": declaration_sha256},
        "source_artifacts": {
            "communication_latency_sha256": latency["content_sha256"],
            "communication_delivery_sha256": delivery["content_sha256"],
            "trace_quality_sha256": quality["content_sha256"],
            "raw_source_set_sha256": source["identity"]["source_set_sha256"]},
        "latency_samples": {"relative_path": "latency_samples.jsonl",
                            "schema_version": "mw_perf.latency_sample.v2",
                            "content_sha256": samples["content_sha256"],
                            "byte_count": len(samples["bytes"]),
                            "record_count": samples["record_count"]},
        "delivery_binding": {"delivery_set_sha256": delivery["delivery_set_sha256"],
                             "record_count": delivery["observed_callback_count"],
                             "complete_exact_set_equality": True},
        "trace_quality_binding": {
            "sample_set_sha256": quality["sample_set_sha256"],
            "delivery_set_sha256": quality["delivery_set_sha256"],
            "stage_record_count": quality["stage_record_count"],
            "exact_record_count": quality["exact_record_count"],
            "zero_loss_exclusion_status_pending_and_io": True},
        "metric": {"measurement": "publish_start_to_callback_start",
                   "start_boundary": "publish_write.start",
                   "end_boundary": "final_callback_invocation.start",
                   "formula": "callback_start_ns-publish_start_ns", "unit": "ns",
                   "clock_domain": "producer_monotonic", "producer": "mw_perf_collect"},
        "classification": {"metric_availability": "supported",
                           "valid_for_metric": True, "valid_for_comparison": False},
        "limitations": list(_LIMITATIONS),
    }
    identity = dict(value); identity.pop("qualification_id")
    value["qualification_id"] = _sha(_ID_DOMAIN + _canonical(identity, newline=False))
    return value


def _schema_validate(value: dict[str, Any]) -> None:
    schema = strict_json_loads(
        (Path(__file__).parent / "schemas/metric-qualification.schema.v1.json").read_bytes())
    if not isinstance(schema, dict) or core._schema_errors(value, schema):
        raise ValueError("metric qualification failed its closed schema")


def _seal_schema_validate(value: dict[str, Any]) -> None:
    schema = strict_json_loads(
        (Path(__file__).parent / "schemas" /
         "metric-qualification-seal.schema.v1.json").read_bytes())
    if not isinstance(schema, dict) or core._schema_errors(value, schema):
        raise ValueError("metric qualification seal failed its closed schema")


def _publication_checkpoint(phase: str, path: Path) -> None:
    """Test hook around the exclusive qualification rename."""


def _seal_read_only(root: Path) -> None:
    """Make a staged qualification content-sealed/read-only and durably sync metadata."""
    directories = [root]
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("qualification contains a symlink")
        if path.is_dir():
            directories.append(path)
        elif path.is_file():
            os.chmod(path, 0o400)
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        else:
            raise ValueError("qualification contains an unsupported node")
    for directory in reversed(directories):
        os.chmod(directory, 0o500)
        fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                     | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def _read_only_modes(root: Path) -> bool:
    if stat.S_IMODE(root.stat(follow_symlinks=False).st_mode) != 0o500:
        return False
    return all(stat.S_IMODE((root / name).stat(follow_symlinks=False).st_mode) == 0o400
               for name in _FILES)


def _remove_owned_destination(path: Path) -> None:
    try:
        os.chmod(path, 0o700)
        for member in path.rglob("*"):
            if member.is_dir() and not member.is_symlink():
                os.chmod(member, 0o700)
            elif member.is_file() and not member.is_symlink():
                os.chmod(member, 0o600)
        shutil.rmtree(path)
    except OSError:
        pass


def build_metric_qualification(campaign_root: Path, *, object_store: Path,
                               registry_path: Path, expected_registry_sha256: str,
                               provider: str, campaign_id: str,
                               destination: Path) -> dict[str, Any]:
    """Build from an operator-approved, externally hash-pinned campaign root."""
    destination = Path(destination)
    if destination.exists():
        raise FileExistsError("qualification destination already exists")
    if not destination.parent.is_dir() or destination.parent.is_symlink():
        raise ValueError("qualification destination parent must be a real directory")
    registry_entry, registry_raw = load_trusted_external_root(
        registry_path, expected_registry_sha256=expected_registry_sha256,
        provider=provider, campaign_id=campaign_id)
    source = _source_derivation(Path(campaign_root), Path(object_store),
                                registry_entry["external_root_sha256"])
    if source["manifest"].get("campaign_id") != campaign_id:
        raise ValueError("registry campaign identity does not match campaign evidence")
    _declaration, declaration_raw = _trusted_declaration()
    value = _qualification(source, _sha(declaration_raw), registry_entry, _sha(registry_raw))
    _schema_validate(value)
    qualification_raw = _canonical(value)
    samples_raw = source["samples"]["bytes"]
    artifact_sha = _sha(_ARTIFACT_DOMAIN + declaration_raw + samples_raw + qualification_raw)
    seal = {"schema": "mw_perf.metric_qualification_seal.v1",
            "qualification_id": value["qualification_id"],
            "artifact_sha256": artifact_sha,
            "declaration_sha256": _sha(declaration_raw),
            "latency_samples_sha256": _sha(samples_raw),
            "metric_qualification_sha256": _sha(qualification_raw), "finalized": True}
    staging: Path | None = None
    owned_destination = False
    try:
        for _ in range(128):
            candidate = destination.parent / f".{destination.name}.{secrets.token_hex(12)}.staging"
            try:
                candidate.mkdir(mode=0o700); staging = candidate; break
            except FileExistsError:
                continue
        if staging is None:
            raise OSError("unable to allocate qualification staging directory")
        for name, data in (("declaration.json", declaration_raw),
                           ("latency_samples.jsonl", samples_raw),
                           ("metric_qualification.json", qualification_raw),
                           ("qualification_seal.json", _canonical(seal))):
            campaign._write_campaign_file(staging, name, data)
        staged_validation = validate_metric_qualification(
            staging, campaign_root=campaign_root, object_store=object_store,
            registry_path=registry_path, expected_registry_sha256=expected_registry_sha256,
            provider=provider, campaign_id=campaign_id, require_read_only=False)
        if staged_validation.get("valid") is not True:
            raise ValueError("staged metric qualification did not revalidate")
        _seal_read_only(staging)
        expected_snapshot = campaign.capture_payload_snapshot(staging)
        _publication_checkpoint("before_rename", staging)
        campaign._rename_noreplace(staging, destination)
        owned_destination = True
        staging = None
        _publication_checkpoint("after_rename", destination)
        published_snapshot = campaign.capture_payload_snapshot(destination)
        if (dict(published_snapshot.files) != dict(expected_snapshot.files)
                or not _read_only_modes(destination)):
            _remove_owned_destination(destination)
            owned_destination = False
            raise ValueError("published metric qualification snapshot changed")
        parent_fd = os.open(destination.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                            | getattr(os, "O_NOFOLLOW", 0))
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
        owned_destination = False
        return {"valid": True, "qualification": value, "seal": seal,
                "qualification_id": value["qualification_id"],
                "artifact_sha256": artifact_sha}
    finally:
        if staging is not None:
            try:
                os.chmod(staging, 0o700)
                for member in staging.rglob("*"):
                    if member.is_file():
                        os.chmod(member, 0o600)
            except OSError:
                pass
            shutil.rmtree(staging, ignore_errors=True)
        if owned_destination:
            _remove_owned_destination(destination)


def validate_metric_qualification(path: Path, *, campaign_root: Path,
                                  object_store: Path, registry_path: Path,
                                  expected_registry_sha256: str, provider: str,
                                  campaign_id: str,
                                  require_read_only: bool = True) -> dict[str, Any]:
    """Revalidate a qualification against its separately supplied trusted registry."""
    try:
        snapshot = campaign.capture_payload_snapshot(Path(path))
        files = dict(snapshot.files)
        if set(files) != _FILES or any(campaign._is_final_campaign_seal(name) for name in files):
            raise ValueError("qualification file set is not exact")
        if require_read_only and not _read_only_modes(Path(path)):
            raise ValueError("qualification is not content-sealed/read-only")
        registry_entry, registry_raw = load_trusted_external_root(
            registry_path, expected_registry_sha256=expected_registry_sha256,
            provider=provider, campaign_id=campaign_id)
        _declaration, declaration_raw = _trusted_declaration()
        if files["declaration.json"] != declaration_raw:
            raise ValueError("qualification declaration is not trusted")
        value = _json(files, "metric_qualification.json")
        seal = _json(files, "qualification_seal.json")
        _schema_validate(value)
        _seal_schema_validate(seal)
        source = _source_derivation(Path(campaign_root), Path(object_store),
                                    registry_entry["external_root_sha256"])
        if source["manifest"].get("campaign_id") != campaign_id:
            raise ValueError("registry campaign identity does not match campaign evidence")
        expected = _qualification(source, _sha(declaration_raw), registry_entry,
                                  _sha(registry_raw))
        if value != expected or files["latency_samples.jsonl"] != source["samples"]["bytes"]:
            raise ValueError("qualification does not match recomputed source evidence")
        qualification_raw = files["metric_qualification.json"]
        samples_raw = files["latency_samples.jsonl"]
        expected_artifact = _sha(
            _ARTIFACT_DOMAIN + declaration_raw + samples_raw + qualification_raw)
        expected_seal = {"schema": "mw_perf.metric_qualification_seal.v1",
            "qualification_id": value["qualification_id"],
            "artifact_sha256": expected_artifact,
            "declaration_sha256": _sha(declaration_raw),
            "latency_samples_sha256": _sha(samples_raw),
            "metric_qualification_sha256": _sha(qualification_raw), "finalized": True}
        if seal != expected_seal:
            raise ValueError("qualification seal is invalid")
        return {"valid": True, "failure_codes": [], "qualification": value,
                "seal": seal, "metric_availability": "supported",
                "valid_for_metric": True, "valid_for_comparison": False,
                "publication": "content-sealed/read-only"}
    except (KeyError, OSError, TypeError, ValueError, UnicodeError,
            json.JSONDecodeError):
        return {"valid": False, "failure_codes": ["METRIC_QUALIFICATION_INVALID"],
                "metric_availability": "unsupported", "valid_for_metric": False,
                "valid_for_comparison": False}
