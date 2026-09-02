import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def make_config(candidate: Path, extension: Path, command: list[str],
                sample_seconds: float) -> dict:
    candidate_sha256 = hashlib.sha256(candidate.read_bytes()).hexdigest()
    extension_sha256 = hashlib.sha256(extension.read_bytes()).hexdigest()
    return {
        "schema_version": 1,
        "validation_profile": "p0a",
        "sut": {"mode": "managed", "command": command},
        "run": {"sample_seconds": sample_seconds, "stop_timeout_seconds": 20},
        "collectors": {
            "process": {"interval_ms": 25, "include_descendants": True},
            "network": {"interfaces": ["lo"]},
        },
        "p0a_evidence": {
            "run_type": "exact_run",
            "registration_contract_version": "mw_perf.p0a_contract.v8",
            "trace_mode": "full",
            "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
            "path_id": "managed-loopback",
            "topology_generation": "frozen-single-process-v1",
            "measurement_kind": "exact_correlated",
            "observer": False,
            "candidate_only": True,
            "delivery_expectations": {
                "publish_count": 1000,
                "readers_per_sample": 1,
                "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records",
            },
            "resolution": {"runtime_images": [
                {"role": "sut", "name": candidate.name, "source": "exe",
                 "expected_path": str(candidate), "sha256": candidate_sha256},
                {"role": "extension", "name": extension.name, "source": "maps",
                 "expected_path": str(extension), "sha256": extension_sha256},
            ]},
        },
        "scenario_contract": {
            "topology": {"processes": 1, "hosts": 1, "transport": "dds-loopback"}
        },
    }


def build_identity(candidate: Path, extension: Path) -> str:
    images = [
        {"role": "sut", "name": candidate.name, "source": "exe",
         "sha256": hashlib.sha256(candidate.read_bytes()).hexdigest()},
        {"role": "extension", "name": extension.name, "source": "maps",
         "sha256": hashlib.sha256(extension.read_bytes()).hexdigest()},
    ]
    images.sort(key=lambda item: (item["role"], item["name"], item["source"], item["sha256"]))
    formula = {"schema": "mw_perf.p0a.fault_build_identity.v1", "runtime_images": images}
    return hashlib.sha256(json.dumps(
        formula, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def run_config(core, output: Path, name: str, config_value: dict):
    config = output / f"{name}.config.json"
    config.write_text(json.dumps(config_value), encoding="utf-8")
    run_root = output / name
    result = core.run(core.argparse.Namespace(
        config=str(config), adapter="generic", command=[],
        override_command=False, output=str(run_root)))
    run_dirs = list(run_root.iterdir())
    assert len(run_dirs) == 1
    return result, run_dirs[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--extension", required=True)
    parser.add_argument("--fault-candidate", required=True)
    parser.add_argument("--fault-extension", required=True)
    parser.add_argument("--production-query-test", required=True)
    parser.add_argument("--source-root", required=True)
    args = parser.parse_args()

    sys.path.insert(0, str(Path(args.source_root) / "tools"))
    from mw_perf_collect import core
    from mw_perf_collect.campaign import (
        build_companion_object_from_snapshot,
        capture_payload_snapshot,
        finalize_campaign,
        finalize_formal_campaign,
        validate_campaign,
        validate_companion_object,
    )
    from mw_perf_collect.formal_anchor import (
        materialize_formal_anchor,
        validate_formal_anchor_snapshot,
    )
    from mw_perf_collect.fault_evaluation import (
        build_evidence_tamper_companion,
        evaluate_evidence_tamper,
    )
    from mw_perf_collect.runtime_fault_evaluation import (
        build_runtime_fault_companion,
        evaluate_runtime_fault_matrix,
    )
    from mw_perf_collect.rollback_evaluation import (
        build_trace_off_rollback_companion,
        execute_trace_off_build,
        validate_trace_off_rollback_companion,
    )

    output = Path(os.environ.get("CTEST_BINARY_DIRECTORY", Path.cwd())) / "managed_exact_candidate_runs"
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    candidate = Path(args.candidate).resolve(strict=True)
    extension = Path(args.extension).resolve(strict=True)
    fault_candidate = Path(args.fault_candidate).resolve(strict=True)
    fault_extension = Path(args.fault_extension).resolve(strict=True)
    production_query_test = Path(args.production_query_test).resolve(strict=True)
    production_build_identity_sha256 = build_identity(candidate, extension)
    fault_build_identity_sha256 = build_identity(fault_candidate, fault_extension)
    assert production_build_identity_sha256 != fault_build_identity_sha256
    assert subprocess.run(
        [str(production_query_test), str(extension)], check=False).returncode == 0
    assert subprocess.run([str(candidate), "--fault=stage_loss"], check=False).returncode == 2
    assert subprocess.run([str(fault_candidate)], check=False).returncode == 2
    result, run_dir = run_config(
        core, output, "runs", make_config(candidate, extension, [str(candidate)], 5))
    assert result == core.EXIT_INVALID
    manifest = json.loads((run_dir / "manifest.json").read_text())
    validation = json.loads((run_dir / "validation.json").read_text())
    evidence = json.loads((run_dir / "capability/p0a_evidence_manifest.json").read_text())
    trace = manifest["dds_comm_trace"]

    assert manifest["run_status"] == "completed"
    assert manifest["candidate_only"] is True
    assert manifest["metric_availability"] == "unsupported"
    assert manifest["measurement_kind"] == "exact_correlated"
    assert "cleanup_error" not in manifest
    assert manifest.get("stop_escalated_sigkill") is not True
    assert trace["status"] == "exact_correlated"
    assert trace["record_count"] == trace["stage_record_count"] == 3000
    assert trace["exact_record_count"] == 1000
    assert {trace[key] for key in (
        "dropped", "invalid_identity", "unsupported_async",
        "missing_registration", "unsupported_multi_handler")} == {0}
    assert (run_dir / "raw/comm_trace/directory_finalization.json").is_file()
    assert validation["valid_for_collection"] is True
    assert validation["valid_for_comparison"] is False
    assert "CANDIDATE_ONLY" in validation["failure_codes"]
    assert evidence["registration_contract_version"] == "mw_perf.p0a_contract.v8"
    assert validation["p0a_evidence"]["valid"] is False
    assert validation["p0a_evidence"]["failure_codes"] == ["P0A_ARTIFACT"]
    resolution = json.loads((run_dir / "capability/resolution.json").read_text())
    assert resolution["schema"] == "mw_perf.p0a.resolution.v2"
    assert resolution["resolved"] is True and resolution["conclusion"] == "pass"
    assert manifest["p0a_resolution"]["collector_written"] is True
    assert manifest["p0a_resolution"]["declaration_sha256"] == resolution["declaration_sha256"]
    identity_path = run_dir / "normalized/identity_trace.jsonl"
    identity_bytes = identity_path.read_bytes()
    identity_records = [json.loads(line) for line in identity_bytes.splitlines()]
    identity_meta = manifest["p0a_identity_trace"]
    assert identity_meta == {
        "collector_written": True,
        "relative_path": "normalized/identity_trace.jsonl",
        "content_sha256": hashlib.sha256(identity_bytes).hexdigest(),
        "source_set_sha256": identity_records[0]["source_set_sha256"],
        "record_count": 1000,
    }
    assert len(identity_records) == 1000

    def sample_key(record):
        return (record["writer_guid"], record["sequence_high"], record["sequence_low"])

    def delivery_key(record):
        return (*sample_key(record), record["reader_guid"],
                record["callback_registration_id"])

    def canonical_delivery_key(record):
        return (bytes.fromhex(record["writer_guid"]), record["sequence_high"],
                record["sequence_low"], bytes.fromhex(record["reader_guid"]),
                record["callback_registration_id"])

    sample_keys = [sample_key(record) for record in identity_records]
    delivery_keys = [delivery_key(record) for record in identity_records]
    assert len(set(sample_keys)) == len(set(delivery_keys)) == 1000
    assert identity_records == sorted(identity_records, key=canonical_delivery_key)
    for record in identity_records:
        assert record["schema"] == "mw_perf.p0a.identity_trace.v2"
        assert record["callback_registration_id"] == 1
        assert (record["publish_start_ns"] <= record["receive_start_ns"]
                and record["receive_end_ns"] <= record["dispatch_start_ns"]
                <= record["callback_start_ns"] <= record["callback_end_ns"]
                <= record["dispatch_end_ns"])
    hook_path = run_dir / "capability/hook_callgraph.json"
    hook_bytes = hook_path.read_bytes()
    hook = json.loads(hook_bytes)
    assert hook["schema"] == "mw_perf.p0a.hook_callgraph.v2"
    assert hook["claim"]["kind"] == "semantic_boundary_chain"
    assert hook["claim"]["dynamic_stack_observation"] == "none"
    assert hook["claim"]["runtime_function_addresses_observed"] is False
    assert hook["claim"]["source_to_build_provenance"] == "not_proven"
    assert hook["stage_record_count"] == 3000
    assert hook["callback_record_count"] == hook["delivery_set"]["count"] == 1000
    assert {item["logical_boundary"]: item["record_count"]
            for item in hook["boundary_records"]} == {
                "publish_write": 1000,
                "take_receive": 1000,
                "sync_dispatch": 1000,
                "final_callback_invocation": 1000,
            }
    assert manifest["p0a_hook_callgraph"]["collector_written"] is True
    assert manifest["p0a_hook_callgraph"]["content_sha256"] == hashlib.sha256(hook_bytes).hexdigest()
    comm_path = run_dir / "normalized/comm_events.jsonl"
    latency_path = run_dir / "normalized/latency_samples.jsonl"
    comm_bytes = comm_path.read_bytes()
    latency_bytes = latency_path.read_bytes()
    comm = [json.loads(line) for line in comm_bytes.splitlines()]
    latency = [json.loads(line) for line in latency_bytes.splitlines()]
    assert len(comm) == 4000 == 1000 + 3 * len(latency)
    assert len(latency) == 1000
    assert {kind: sum(item["event_kind"] == kind for item in comm)
            for kind in ("publish", "receive", "dispatch", "callback")} == {
                "publish": 1000, "receive": 1000,
                "dispatch": 1000, "callback": 1000}
    assert [item["event_kind"] for item in comm] == \
        [kind for _ in range(1000)
         for kind in ("publish", "receive", "dispatch", "callback")]
    assert [sample_key(comm[index]) for index in range(0, len(comm), 4)] == sample_keys
    assert [delivery_key(item) for item in latency] == delivery_keys
    for item in latency:
        assert item["measurement"] == "publish_start_to_callback_start"
        assert item["latency_ns"] == item["callback_start_ns"] - item["publish_start_ns"]
    for name, raw, records in (("p0a_comm_events", comm_bytes, comm),
                               ("p0a_latency_samples", latency_bytes, latency)):
        metadata = manifest[name]
        assert metadata["collector_written"] is True
        assert metadata["content_sha256"] == hashlib.sha256(raw).hexdigest()
        assert metadata["source_identity_sha256"] == identity_meta["content_sha256"]
        assert metadata["source_identity_record_count"] == 1000
        assert metadata["record_count"] == len(records)
    capability_path = run_dir / "capability/communication_latency.json"
    capability_bytes = capability_path.read_bytes()
    capability = json.loads(capability_bytes)
    capability_meta = manifest["p0a_communication_latency"]
    assert capability["schema"] == "mw_perf.communication_latency.v2"
    assert capability["metric"] == {
        "measurement": "publish_start_to_callback_start",
        "start_boundary": "publish_write.start",
        "end_boundary": "final_callback_invocation.start",
        "formula": "callback_start_ns-publish_start_ns",
        "unit": "ns", "clock_domain": "producer_monotonic"}
    assert capability["sample_set"]["sha256"] == hook["delivery_set"]["sha256"]
    assert capability["sample_set"]["count"] == hook["delivery_set"]["count"] == 1000
    assert capability["support"] == {"level": "candidate_run_only", "aggregation": "none",
        "delivery_claim": "none", "performance_claim": "none",
        "metric_availability": "unsupported", "valid_for_comparison": False}
    assert capability["conclusion_scope"] == "run_scoped_metric_artifact_integrity"
    assert capability["conclusion"] == "pass" and "stats" not in capability
    assert capability_meta == {
        "collector_written": True,
        "relative_path": "capability/communication_latency.json",
        "content_sha256": hashlib.sha256(capability_bytes).hexdigest(),
        "latency_samples_content_sha256": hashlib.sha256(latency_bytes).hexdigest(),
        "latency_samples_record_count": 1000,
        "sample_set_sha256": hook["delivery_set"]["sha256"],
        "sample_set_count": 1000}
    capability_entry = next(item for item in evidence["entries"]
                            if item["relative_path"] ==
                            "capability/communication_latency.json")
    assert capability_entry["sha256"] == hashlib.sha256(capability_bytes).hexdigest()
    assert capability_entry["record_count"] == 1
    assert capability_entry["schema_version"] == "mw_perf.communication_latency.v2"
    delivery_path = run_dir / "validation/communication_delivery.json"
    delivery_bytes = delivery_path.read_bytes()
    delivery = json.loads(delivery_bytes)
    assert delivery["schema"] == "mw_perf.communication_delivery.v2"
    assert delivery["expectations"] == {
        "publish_count": 1000, "readers_per_sample": 1,
        "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records"}
    assert delivery["callback_order_basis"] == "exact_record_submission_at_callback_exit"
    assert delivery["callback_entry_order_claim"] == "none"
    assert delivery["correctness"]["callback_completion_record_order_monotonic"] is True
    assert delivery["observed"]["publish"] == 1000
    assert delivery["observed"]["delivery_set"]["sha256"] == hook["delivery_set"]["sha256"]
    assert manifest["p0a_communication_delivery"]["content_sha256"] == \
        hashlib.sha256(delivery_bytes).hexdigest()
    delivery_entry = next(item for item in evidence["entries"]
                          if item["relative_path"] ==
                          "validation/communication_delivery.json")
    assert delivery_entry["sha256"] == hashlib.sha256(delivery_bytes).hexdigest()
    assert delivery_entry["schema_version"] == "mw_perf.communication_delivery.v2"
    revalidated_trace = validation["p0a_evidence"]["dds_comm_trace"]
    assert all(revalidated_trace[key] == value for key, value in trace.items())

    tampered_hook = dict(hook)
    tampered_hook["delivery_set"] = dict(hook["delivery_set"])
    tampered_hook["delivery_set"]["sha256"] = "0" * 64
    tampered_bytes = json.dumps(
        tampered_hook, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    hook_path.write_bytes(tampered_bytes)
    tampered_digest = hashlib.sha256(tampered_bytes).hexdigest()
    tampered_evidence = json.loads(json.dumps(evidence))
    for group in (tampered_evidence["entries"], tampered_evidence["required"]):
        next(item for item in group
             if item["relative_path"] == "capability/hook_callgraph.json")["sha256"] = \
            tampered_digest
    tampered_root = json.loads(json.dumps(manifest))
    tampered_root["p0a_hook_callgraph"]["content_sha256"] = tampered_digest
    tampered_root["p0a_hook_callgraph"]["delivery_set_sha256"] = "0" * 64
    (run_dir / "manifest.json").write_text(json.dumps(tampered_root))
    rejected = core.validate_p0a_evidence_manifest(
        run_dir, tampered_evidence, producer_pid=manifest["pid"],
        producer_start_time_ticks=manifest["pid_start_time_ticks"],
        topology_generation=trace["topology_generation"],
        topology_sha256=trace["topology_sha256"])
    assert "P0A_HOOK_CALLGRAPH" in rejected["failure_codes"]
    hook_path.write_bytes(hook_bytes)
    (run_dir / "manifest.json").write_text(json.dumps(manifest))

    tampered_comm = comm_bytes.replace(b'"finalized":true', b'"finalized":false', 1)
    comm_path.write_bytes(tampered_comm)
    tampered_evidence = json.loads(json.dumps(evidence))
    tampered_digest = hashlib.sha256(tampered_comm).hexdigest()
    for group in (tampered_evidence["entries"], tampered_evidence["required"]):
        next(item for item in group
             if item["relative_path"] == "normalized/comm_events.jsonl")["sha256"] = \
            tampered_digest
    tampered_root = json.loads(json.dumps(manifest))
    tampered_root["p0a_comm_events"]["content_sha256"] = tampered_digest
    (run_dir / "manifest.json").write_text(json.dumps(tampered_root))
    rejected = core.validate_p0a_evidence_manifest(
        run_dir, tampered_evidence, producer_pid=manifest["pid"],
        producer_start_time_ticks=manifest["pid_start_time_ticks"],
        topology_generation=trace["topology_generation"],
        topology_sha256=trace["topology_sha256"])
    assert "P0A_NORMALIZED_RESULTS" in rejected["failure_codes"]
    comm_path.write_bytes(comm_bytes)
    (run_dir / "manifest.json").write_text(json.dumps(manifest))

    tampered_capability = json.loads(json.dumps(capability))
    tampered_capability["sample_set"]["sha256"] = "0" * 64
    tampered_bytes = json.dumps(
        tampered_capability, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    capability_path.write_bytes(tampered_bytes)
    tampered_digest = hashlib.sha256(tampered_bytes).hexdigest()
    tampered_evidence = json.loads(json.dumps(evidence))
    for group in (tampered_evidence["entries"], tampered_evidence["required"]):
        next(item for item in group if item["relative_path"] ==
             "capability/communication_latency.json")["sha256"] = tampered_digest
    tampered_root = json.loads(json.dumps(manifest))
    tampered_root["p0a_communication_latency"]["content_sha256"] = tampered_digest
    tampered_root["p0a_communication_latency"]["sample_set_sha256"] = "0" * 64
    (run_dir / "manifest.json").write_text(json.dumps(tampered_root))
    rejected = core.validate_p0a_evidence_manifest(
        run_dir, tampered_evidence, producer_pid=manifest["pid"],
        producer_start_time_ticks=manifest["pid_start_time_ticks"],
        topology_generation=trace["topology_generation"],
        topology_sha256=trace["topology_sha256"])
    assert "P0A_COMMUNICATION_LATENCY" in rejected["failure_codes"]
    capability_path.write_bytes(capability_bytes)
    (run_dir / "manifest.json").write_text(json.dumps(manifest))

    tampered_root = json.loads(json.dumps(manifest))
    tampered_root["p0a_communication_latency"]["sample_set_sha256"] = "0" * 64
    (run_dir / "manifest.json").write_text(json.dumps(tampered_root))
    rejected = core.validate_p0a_evidence_manifest(
        run_dir, evidence, producer_pid=manifest["pid"],
        producer_start_time_ticks=manifest["pid_start_time_ticks"],
        topology_generation=trace["topology_generation"],
        topology_sha256=trace["topology_sha256"])
    assert "P0A_COMMUNICATION_LATENCY" in rejected["failure_codes"]
    (run_dir / "manifest.json").write_text(json.dumps(manifest))

    tampered_evidence = json.loads(json.dumps(evidence))
    for group in (tampered_evidence["entries"], tampered_evidence["required"]):
        next(item for item in group if item["relative_path"] ==
             "capability/communication_latency.json")["sha256"] = "0" * 64
    rejected = core.validate_p0a_evidence_manifest(
        run_dir, tampered_evidence, producer_pid=manifest["pid"],
        producer_start_time_ticks=manifest["pid_start_time_ticks"],
        topology_generation=trace["topology_generation"],
        topology_sha256=trace["topology_sha256"])
    assert "P0A_COMMUNICATION_LATENCY" in rejected["failure_codes"]

    trace_names = {path.name for path in (run_dir / "raw/comm_trace").iterdir()}
    assert any(name.startswith("ibmw_dds_trace.") and name.endswith(".meta.json")
               for name in trace_names)
    assert any(name.startswith("ibmw_dds_exact.") and name.endswith(".exact.meta.json")
               for name in trace_names)
    stdout = (run_dir / "logs/sut.stdout.log").read_text()
    stderr = (run_dir / "logs/sut.stderr.log").read_text()
    assert "managed exact candidate ready" in stdout
    assert "startup failure" not in stdout.lower() + stderr.lower()
    assert not any((run_dir / relative).exists() for relative in (
        "validation/fault_injection.json", "validation/rollback.json"))
    missing = {item["relative_path"] for item in evidence["required"]
               if item["sha256"] is None}
    assert missing == {"validation/fault_injection.json", "validation/rollback.json"}
    assert "candidate only: `True`" in (run_dir / "report.md").read_text()

    anchor_manifest_bytes = (run_dir / "manifest.json").read_bytes()
    tamper_targets = {
        "capability/hook_callgraph.json": "P0A_HOOK_CALLGRAPH",
        "capability/communication_latency.json": "P0A_COMMUNICATION_LATENCY",
        "validation/communication_delivery.json": "P0A_COMMUNICATION_DELIVERY",
        "normalized/identity_trace.jsonl": "P0A_IDENTITY_TRACE",
    }
    tamper_reports = {}
    tamper_experiments = {}
    (output / "evidence_tamper").mkdir()
    for target_relative, expected_code in tamper_targets.items():
        experiment = output / "evidence_tamper" / target_relative.replace("/", "_")
        report = evaluate_evidence_tamper(run_dir, experiment, target_relative)
        assert report["finalized"] is True
        assert report["conclusion"] == "pass"
        assert report["validation"]["expected_new_p0a_code"] == expected_code
        assert expected_code in report["validation"]["observed_p0a_codes"]
        tamper_reports[target_relative] = report
        tamper_experiments[target_relative] = experiment

    campaign_id = "managed-exact-candidate-" + production_build_identity_sha256[:20]
    canonical_target = "validation/communication_delivery.json"
    tamper_descriptor = build_evidence_tamper_companion(
        tamper_reports[canonical_target], tamper_experiments[canonical_target],
        campaign_id=campaign_id,
        companion_id="communication-delivery-byte-xor",
        build_identity_sha256=production_build_identity_sha256,
        object_store=output / "campaign_objects")
    assert tamper_descriptor["campaign_id"] == campaign_id
    assert tamper_descriptor["role"] == "fault_evidence_tamper"
    assert tamper_descriptor["build_identity_sha256"] == production_build_identity_sha256
    object_path = (output / "campaign_objects" / "sha256" /
                   tamper_descriptor["object_sha256"][:2] /
                   f'{tamper_descriptor["object_sha256"]}.tar')
    object_validation = validate_companion_object(object_path, tamper_descriptor)
    assert object_validation["valid"] is True
    assert object_validation["descriptor"] == tamper_descriptor

    assert (run_dir / "manifest.json").read_bytes() == anchor_manifest_bytes
    final_validation = json.loads((run_dir / "validation.json").read_text())
    assert final_validation["p0a_evidence"]["failure_codes"] == ["P0A_ARTIFACT"]
    assert not any((run_dir / relative).exists() for relative in (
        "validation/fault_injection.json", "validation/rollback.json"))

    fault_subjects = {}
    expected_error = {
        "stage_loss": "DDS trace required zero-loss finalization failed",
        "callback_loss": "exact seal is not finalized",
        "producer_identity_mismatch": "DDS trace identity mismatch: run_id",
        "stage_status": "exact correlation requires zero status for every event",
        "callback_status": "exact correlation requires zero status for every event",
    }
    for case_id in sorted(expected_error):
        fault_result, subject = run_config(
            core, output, f"fault_{case_id}",
            make_config(fault_candidate, fault_extension,
                        [str(fault_candidate), f"--fault={case_id}"], 1.5))
        assert fault_result == core.EXIT_INVALID
        fault_manifest = json.loads((subject / "manifest.json").read_text())
        fault_validation = json.loads((subject / "validation.json").read_text())
        assert fault_manifest["run_status"] == "invalid"
        assert fault_manifest["sut_returncode"] == 0
        assert fault_manifest["dds_comm_trace"]["status"] == "invalid"
        assert expected_error[case_id] in fault_manifest["dds_comm_trace"]["error"]
        assert "cleanup_error" not in fault_manifest
        assert fault_manifest.get("stop_escalated_sigkill") is not True
        assert not (subject / "raw/comm_trace/directory_finalization.json").exists()
        assert fault_validation["valid_for_metric"] is False
        assert fault_validation["valid_for_comparison"] is False
        assert "P0A_DDS_COMM_TRACE" in fault_validation["p0a_evidence"]["failure_codes"]
        fault_subjects[case_id] = subject

    matrix_experiment = output / "runtime_fault_matrix"
    matrix_report = evaluate_runtime_fault_matrix(
        fault_subjects, matrix_experiment, run_dir)
    assert matrix_report["conclusion"] == "pass"
    assert matrix_report["fault_build_identity_sha256"] == fault_build_identity_sha256
    assert matrix_report["production_build_identity_sha256"] == \
        production_build_identity_sha256
    assert matrix_report["production_build_identity_sha256"] != \
        matrix_report["fault_build_identity_sha256"]
    assert matrix_report["campaign_classification"] == {
        "classification": "candidate", "metric_availability": "unsupported",
        "valid_for_comparison": False, "failure_code": "P0A_ARTIFACT"}
    assert matrix_report["instrumentation_relationship"] == \
        "test_instrumented_fault_producer_not_binary_identical_to_anchor"
    assert matrix_report["evidence_claim"] == \
        "collector_fail_closed_on_deterministic_test_instrumented_producer_anomalies"
    assert matrix_report["anchor"]["run_id"] == manifest["run_id"]
    assert final_validation["p0a_evidence"]["failure_codes"] == ["P0A_ARTIFACT"]
    assert [item["case_id"] for item in matrix_report["cases"]] == sorted(expected_error)
    matrix_descriptor = build_runtime_fault_companion(
        matrix_report, matrix_experiment, campaign_id=campaign_id,
        companion_id="managed-runtime-fault-matrix",
        object_store=output / "campaign_objects")
    assert matrix_descriptor["role"] == "fault_runtime_matrix"
    assert matrix_descriptor["build_identity_sha256"] == fault_build_identity_sha256
    matrix_object = (output / "campaign_objects" / "sha256" /
                     matrix_descriptor["object_sha256"][:2] /
                     f'{matrix_descriptor["object_sha256"]}.tar')
    assert validate_companion_object(matrix_object, matrix_descriptor)["valid"] is True
    assert (run_dir / "manifest.json").read_bytes() == anchor_manifest_bytes

    rollback_result = execute_trace_off_build(
        Path(args.source_root), Path(os.environ["CTEST_BINARY_DIRECTORY"]),
        output / "rollback_work")
    rollback = build_trace_off_rollback_companion(
        rollback_result, run_dir, output / "rollback_payload",
        output / "campaign_objects", campaign_id,
        "fresh-trace-off-business-roundtrip")
    rollback_descriptor = rollback["descriptor"]
    assert rollback["report"]["classification"] == {
        "classification": "candidate", "metric_availability": "unsupported",
        "valid_for_comparison": False, "failure_code": "P0A_ARTIFACT"}
    assert rollback_descriptor["role"] == "rollback_trace_off"
    rollback_object = (output / "campaign_objects" / "sha256" /
                       rollback_descriptor["object_sha256"][:2] /
                       f'{rollback_descriptor["object_sha256"]}.tar')
    assert validate_trace_off_rollback_companion(
        rollback_object, rollback_descriptor)["valid"] is True
    assert (run_dir / "manifest.json").read_bytes() == anchor_manifest_bytes
    assert json.loads((run_dir / "validation.json").read_text())[
        "p0a_evidence"]["failure_codes"] == ["P0A_ARTIFACT"]

    contract_bytes = (run_dir / "registration/contract.json").read_bytes()
    exact_descriptor = build_companion_object_from_snapshot(
        capture_payload_snapshot(run_dir), output / "campaign_objects", {
            "campaign_id": campaign_id,
            "role": "exact_anchor",
            "companion_id": "managed-exact-anchor",
            "run_id": manifest["run_id"],
            "evidence_contract": {
                "id": evidence["registration_contract_version"],
                "sha256": hashlib.sha256(contract_bytes).hexdigest(),
            },
            "root_manifest_sha256": hashlib.sha256(anchor_manifest_bytes).hexdigest(),
            "evidence_manifest_sha256": hashlib.sha256(
                (run_dir / "capability/p0a_evidence_manifest.json").read_bytes()).hexdigest(),
            "build_identity_sha256": production_build_identity_sha256,
        })
    campaign = finalize_campaign(
        output / "finalized_campaign", campaign_id=campaign_id,
        descriptors={
            "exact_anchor": exact_descriptor,
            "fault_evidence_tamper": tamper_descriptor,
            "fault_runtime_matrix": matrix_descriptor,
            "rollback_trace_off": rollback_descriptor,
        },
        object_store=output / "campaign_objects")
    assert campaign["validation"]["valid"] is True
    assert campaign["manifest"]["classification"] == {
        "classification": "candidate", "metric_availability": "unsupported",
        "valid_for_comparison": False, "failure_code": "P0A_ARTIFACT"}
    assert campaign["seal"]["finalized"] is True
    assert campaign["seal"]["campaign_root_sha256"] == campaign["campaign_root_sha256"]
    verified_campaign = validate_campaign(
        output / "finalized_campaign", object_store=output / "campaign_objects",
        expected_campaign_root_sha256=campaign["campaign_root_sha256"])
    assert verified_campaign["valid"] is True
    assert [item["role"] for item in campaign["manifest"]["registrations"]] == [
        "exact_anchor", "fault_evidence_tamper", "fault_runtime_matrix",
        "rollback_trace_off"]
    assert (run_dir / "manifest.json").read_bytes() == anchor_manifest_bytes

    exact_object = (output / "campaign_objects" / "sha256" /
                    exact_descriptor["object_sha256"][:2] /
                    f'{exact_descriptor["object_sha256"]}.tar')
    exact_object_bytes = exact_object.read_bytes()
    companion_descriptors = {
        "fault_evidence_tamper": tamper_descriptor,
        "fault_runtime_matrix": matrix_descriptor,
        "rollback_trace_off": rollback_descriptor,
    }
    formal_anchor = materialize_formal_anchor(
        stage1_descriptor=exact_descriptor,
        stage1_object_path=exact_object,
        companion_descriptors=companion_descriptors,
        object_store=output / "campaign_objects",
        destination=output / "formal_anchor",
        campaign_id=campaign_id,
        companion_id="managed-formal-anchor-v9")
    formal_descriptor = formal_anchor["formal_descriptor"]
    assert formal_anchor["source_descriptor"] == exact_descriptor
    assert formal_descriptor["campaign_id"] == campaign_id
    assert formal_descriptor["role"] == "exact_anchor"
    assert formal_descriptor["companion_id"] == "managed-formal-anchor-v9"
    assert formal_descriptor["run_id"] == exact_descriptor["run_id"]
    assert formal_descriptor["build_identity_sha256"] == \
        exact_descriptor["build_identity_sha256"]
    assert formal_descriptor["evidence_contract"]["id"] == \
        "mw_perf.p0a_contract.v9"
    assert exact_descriptor["evidence_contract"]["id"] == \
        "mw_perf.p0a_contract.v8"
    assert all(descriptor["evidence_contract"] ==
               exact_descriptor["evidence_contract"]
               for descriptor in companion_descriptors.values())

    formal_snapshot_validation = validate_formal_anchor_snapshot(
        output / "formal_anchor")
    formal_document = json.loads((
        output / "formal_anchor/validation/formal_anchor.json").read_text())
    formal_validation_document = json.loads((
        output / "formal_anchor/validation.json").read_text())
    formal_manifest_bytes = (
        output / "formal_anchor/manifest.json").read_bytes()
    formal_evidence_bytes = (
        output / "formal_anchor/capability/p0a_evidence_manifest.json").read_bytes()
    formal_contract_bytes = (
        output / "formal_anchor/registration/contract.json").read_bytes()
    assert formal_snapshot_validation == {
        "valid": True,
        "formal_anchor_sha256": formal_anchor["formal_anchor_sha256"],
        "formal_anchor_id": formal_document["formal_anchor_id"],
    }
    assert formal_descriptor["root_manifest_sha256"] == \
        hashlib.sha256(formal_manifest_bytes).hexdigest()
    assert formal_descriptor["evidence_manifest_sha256"] == \
        hashlib.sha256(formal_evidence_bytes).hexdigest()
    assert formal_descriptor["evidence_contract"]["sha256"] == \
        hashlib.sha256(formal_contract_bytes).hexdigest()
    assert formal_document["source_anchor"] == exact_descriptor
    assert formal_document["companions"] == companion_descriptors
    assert formal_document["registration_contract"]["version"] == \
        "mw_perf.p0a_contract.v9"
    assert formal_document["status"] == {
        "valid_for_p0a_completion": True,
        "metric_availability": "unsupported",
        "valid_for_comparison": False,
    }
    assert formal_document["finalized"] is True
    assert formal_document["conclusion"] == "pass"
    assert formal_validation_document["valid_for_p0a_completion"] is True
    assert formal_validation_document["valid_for_metric"] is False
    assert formal_validation_document["valid_for_comparison"] is False
    assert "P0A_ARTIFACT" not in json.dumps(
        formal_validation_document, sort_keys=True)
    assert exact_object.read_bytes() == exact_object_bytes
    assert (run_dir / "manifest.json").read_bytes() == anchor_manifest_bytes

    formal_campaign = finalize_formal_campaign(
        output / "finalized_formal_campaign", campaign_id=campaign_id,
        descriptors={"exact_anchor": formal_descriptor,
                     **companion_descriptors},
        retained_v8_source_descriptor=exact_descriptor,
        object_store=output / "campaign_objects")
    verified_formal_campaign = validate_campaign(
        output / "finalized_formal_campaign",
        object_store=output / "campaign_objects",
        expected_campaign_root_sha256=formal_campaign["campaign_root_sha256"])
    assert verified_formal_campaign["valid"] is True
    assert verified_formal_campaign["failure_codes"] == []
    assert formal_campaign["manifest"]["schema"] == \
        "mw_perf.evidence_campaign.v2"
    assert formal_campaign["validation"]["schema"] == \
        "mw_perf.evidence_campaign_validation.v2"
    assert formal_campaign["seal"]["schema"] == \
        "mw_perf.evidence_campaign_seal.v2"
    formal_registrations = formal_campaign["manifest"]["registrations"]
    assert [item["role"] for item in formal_registrations] == [
        "exact_anchor", "fault_evidence_tamper", "fault_runtime_matrix",
        "rollback_trace_off"]
    assert formal_campaign["manifest"]["retained_v8_source_descriptor"] == \
        exact_descriptor
    assert len(formal_campaign["validation"]["checks"]) == 5
    assert [(item["role"], item["check_type"])
            for item in formal_campaign["validation"]["checks"]] == [
        ("exact_anchor", "registration"),
        ("fault_evidence_tamper", "registration"),
        ("fault_runtime_matrix", "registration"),
        ("rollback_trace_off", "registration"),
        ("exact_anchor", "retained_v8_source"),
    ]
    assert formal_campaign["manifest"]["classification"] == {
        "valid_for_p0a_completion": True,
        "metric_availability": "unsupported",
        "valid_for_comparison": False,
    }
    assert formal_campaign["validation"]["classification"] == \
        formal_campaign["manifest"]["classification"]
    assert formal_campaign["seal"]["finalized"] is True
    assert formal_campaign["seal"]["campaign_root_sha256"] == \
        formal_campaign["campaign_root_sha256"]
    public_descriptors = {
        item["role"]: item["descriptor"] for item in formal_registrations}
    assert public_descriptors["exact_anchor"] == formal_descriptor
    assert public_descriptors["exact_anchor"]["evidence_contract"]["id"] == \
        "mw_perf.p0a_contract.v9"
    assert all(public_descriptors[role]["evidence_contract"] ==
               exact_descriptor["evidence_contract"]
               for role in companion_descriptors)
    assert "P0A_ARTIFACT" not in json.dumps({
        "manifest": formal_campaign["manifest"],
        "validation": formal_campaign["validation"],
        "seal": formal_campaign["seal"],
    }, sort_keys=True)
    assert exact_object.read_bytes() == exact_object_bytes
    assert (run_dir / "manifest.json").read_bytes() == anchor_manifest_bytes
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
