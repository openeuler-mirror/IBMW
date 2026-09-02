import json

from mw_perf_collect.adapters.ros2 import (
    archive_latency_artifact,
    latency_artifact_snapshot,
    validate_latency_artifact,
)


def record(seq=0, **changes):
    value = {"schema": "mw_perf.latency.v1", "seq": seq, "send_ns": 10 + seq,
             "receive_ns": 20 + seq, "latency_ns": 10, "topic": "/chatter",
             "type": "std_msgs/msg/String", "payload_bytes": 4, "valid": True}
    value.update(changes)
    return value


def completion(count=2, run_id="run", pid=1, start_ticks=2, topic="/chatter",
               message_type="std_msgs/msg/String", **changes):
    value = {"schema": "mw_perf.latency.v1", "kind": "completion", "run_id": run_id,
             "producer_pid": pid, "producer_start_time_ticks": start_ticks,
             "record_count": count, "topic": topic, "type": message_type, "complete": True,
             "clock_domain": "producer_monotonic"}
    value.update(changes)
    return value


def write(path, records, newline=True):
    path.write_text("\n".join(json.dumps(item) for item in records) + ("\n" if newline else ""))


def test_latency_contract_accepts_valid_single_topic_artifact(tmp_path):
    path = tmp_path / "latency.jsonl"; write(path, [record(0), record(1), completion()])
    result = validate_latency_artifact(path)
    assert result["valid"] and result["record_count"] == 2


def test_latency_contract_rejects_truncated_duplicate_formula_and_multi_topic(tmp_path):
    cases = {
        "truncated": ([record()], False),
        "duplicate": ([record(0), record(0), completion()], True),
        "formula": ([record(latency_ns=9), completion(count=1)], True),
        "multi": ([record(0), record(1, topic="/other"), completion()], True),
        "nan": ([record(payload_bytes=float("nan")), completion(count=1)], True),
    }
    for name, (records, newline) in cases.items():
        path = tmp_path / f"{name}.jsonl"; write(path, records, newline)
        assert validate_latency_artifact(path)["valid"] is False, name


def test_archive_latency_fresh_managed_and_stale(tmp_path):
    source = tmp_path / "producer.jsonl"; write(source, [record(), completion(count=1)])
    baseline = latency_artifact_snapshot(source)
    stale = archive_latency_artifact(source, baseline, tmp_path / "stale.jsonl", managed=True,
        graceful_finalization=True, run_id="run", mode="managed", sut_pid=1, sut_start_time_ticks=2)
    assert stale["status"] == "stale"
    write(source, [record(), record(1), completion()])
    result = archive_latency_artifact(source, baseline, tmp_path / "raw" / "latency.jsonl", managed=True,
        graceful_finalization=True, run_id="run", mode="managed", sut_pid=1, sut_start_time_ticks=2)
    assert result["status"] == "supported"
    assert result["provenance"]["source_sha256"] == result["provenance"]["archive_sha256"]


def test_attached_required_fails_closed_optional_is_diagnostic(tmp_path):
    source = tmp_path / "latency.jsonl"; baseline = latency_artifact_snapshot(source); write(source, [record(), completion(count=1)])
    optional = archive_latency_artifact(source, baseline, tmp_path / "optional.jsonl", managed=False,
        graceful_finalization=False, run_id="run", mode="attached", sut_pid=1, sut_start_time_ticks=2)
    required = archive_latency_artifact({"path": str(source), "required": True}, baseline,
        tmp_path / "required.jsonl", managed=False, graceful_finalization=False, run_id="run",
        mode="attached", sut_pid=1, sut_start_time_ticks=2)
    assert optional["status"] == "partial"
    assert required["status"] == "invalid"


def test_latency_identity_and_completion_are_bound(tmp_path):
    path = tmp_path / "latency.jsonl"
    write(path, [record(), completion(count=1, run_id="other")])
    assert not validate_latency_artifact(path, expected_run_id="run", expected_producer_pid=1,
                                        expected_producer_start_time_ticks=2)["valid"]
    write(path, [record(), completion(count=2)])
    assert not validate_latency_artifact(path)["valid"]


def test_latency_jsonl_rejects_duplicate_nested_object_keys(tmp_path):
    path = tmp_path / "latency.jsonl"
    path.write_text('{"schema":"mw_perf.latency.v1","seq":0,"seq":1}\n')
    assert not validate_latency_artifact(path)["valid"]


def test_legacy_completion_is_readable_but_ineligible(tmp_path):
    path = tmp_path / "legacy.jsonl"
    trailer = completion(count=1); trailer.pop("clock_domain")
    write(path, [record(), trailer])
    result = validate_latency_artifact(path)
    assert result["valid"] and result["legacy"]
    archived = archive_latency_artifact(path, latency_artifact_snapshot(tmp_path / "missing"),
        tmp_path / "archive.jsonl", managed=True, graceful_finalization=True,
        run_id="run", mode="managed", sut_pid=1, sut_start_time_ticks=2)
    assert archived["status"] == "partial"


def test_unknown_clock_domain_fails_closed(tmp_path):
    path = tmp_path / "unknown.jsonl"
    write(path, [record(), completion(count=1, clock_domain="wall_clock")])
    assert not validate_latency_artifact(path)["valid"]
