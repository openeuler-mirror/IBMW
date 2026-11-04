import hashlib
import json
import sys
from pathlib import Path

import mw_perf_collect.core as core
from mw_perf_collect.adapters.csv_latency import archive, revalidate, snapshot, validate_csv
from mw_perf_collect.core import _schema_errors


HEADER = "label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\n"


def _csv(label="abc_ros2_4KB", count=3000, values="1,2,1,1,2,3,4,5"):
    return HEADER + f"{label},{count},{values}\n"


def _config(root):
    return {"schema": "mw_perf.latency.csv.v1", "format": "LatencyStats CSV",
            "path": str(root / "source.csv"), "result_path": str(root / "result.json"),
            "required": True, "lane": "A", "expected_label": "abc_ros2_4KB",
            "expected_count": 3000}


def _write_sources(root, *, run_id="run", pid=10, ticks=20, valid=True):
    csv_path = root / "source.csv"; csv_path.write_text(_csv())
    digest = hashlib.sha256(csv_path.read_bytes()).hexdigest()
    result = {"lane": "A", "valid": valid, "collector_managed": {
        "schema": "ibmw.abc.collector-managed.v1", "run_id": run_id,
        "producer_pid": pid, "producer_start_time_ticks": ticks, "lane": "A",
        "complete": True, "latency_csv_sha256": digest}}
    (root / "result.json").write_text(json.dumps(result))


def test_csv_rejects_malformed_negative_nonfinite_and_bad_quantiles(tmp_path):
    cases = ("bad\n", _csv(values="-1,2,1,1,2,3,4,5"),
             _csv(values="1,nan,1,1,2,3,4,5"), _csv(values="1,2,1,3,2,3,4,5"))
    for index, text in enumerate(cases):
        path = tmp_path / str(index); path.write_text(text)
        assert validate_csv(path, "abc_ros2_4KB", 3000)["valid"] is False


def test_config_schema_requires_versioned_csv_and_managed_resources():
    schema = json.loads((Path(__file__).parents[1] / "schemas/config.schema.v1.json").read_text())
    value = {"sut": {"mode": "managed", "command": ["/bin/true"]},
        "collectors": {"process": {"include_descendants": True}, "network": {"interfaces": ["lo"]}},
        "comparison": {"schema": "mw_perf.comparison-metadata.v1", "contract": "c", "profile": "p",
            "workload": "w", "resource_scope": "r", "order": ["A", "B", "C"],
            "lane": "A", "block": "b", "sequence_index": 0},
        "artifacts": {"latency": _config(Path("/tmp"))}}
    assert not _schema_errors(value, schema)
    value["artifacts"]["latency"]["schema"] = "mw_perf.latency.v0"
    assert _schema_errors(value, schema)


def test_config_schema_binds_lane_set_to_metadata_version():
    schema = json.loads((Path(__file__).parents[1] / "schemas/config.schema.v1.json").read_text())
    def config(metadata, lane, order, artifact_lane=None):
        latency = {**_config(Path("/tmp")), "lane": artifact_lane or lane}
        return {"sut": {"mode": "managed", "command": ["/bin/true"]},
            "collectors": {"process": {"include_descendants": True}, "network": {"interfaces": ["lo"]}},
            "comparison": {"schema": f"mw_perf.comparison-metadata.{metadata}", "contract": "c",
                "profile": "p", "workload": "w", "resource_scope": "r", "order": order,
                "lane": lane, "block": "b", "sequence_index": 0},
            "artifacts": {"latency": latency}}
    assert not _schema_errors(config("v2", "S", ["S", "A", "B"]), schema)
    assert not _schema_errors(config("v2", "A", ["A", "B", "S"]), schema)
    assert not _schema_errors(config("v1", "C", ["C", "A", "B"]), schema)
    assert _schema_errors(config("v1", "S", ["A", "B", "C"]), schema)
    assert _schema_errors(config("v1", "A", ["A", "B", "S"]), schema)
    assert _schema_errors(config("v2", "C", ["A", "B", "S"]), schema)
    assert _schema_errors(config("v2", "S", ["A", "B", "C"]), schema)
    assert _schema_errors(config("v1", "A", ["A", "B", "C"], artifact_lane="S"), schema)
    assert _schema_errors(config("v2", "A", ["A", "B", "S"], artifact_lane="C"), schema)
    without_comparison = config("v1", "A", ["A", "B", "C"], artifact_lane="S")
    del without_comparison["comparison"]
    assert _schema_errors(without_comparison, schema)


def test_archive_rejects_stale_hash_and_identity(tmp_path):
    config = _config(tmp_path); _write_sources(tmp_path)
    baseline = snapshot(config)
    stale = archive(config, baseline, tmp_path / "stale/latency.csv", tmp_path / "stale/result.json",
                    run_id="run", mode="managed", pid=10, start_ticks=20,
                    graceful_finalization=True)
    assert stale["status"] == "invalid"
    for name, mutation in (("run", lambda value: value["collector_managed"].update(run_id="other")),
                           ("pid", lambda value: value["collector_managed"].update(producer_pid=11)),
                           ("hash", lambda value: value["collector_managed"].update(latency_csv_sha256="0" * 64))):
        case = tmp_path / name; case.mkdir(); cfg = _config(case); base = snapshot(cfg)
        _write_sources(case); result = json.loads((case / "result.json").read_text()); mutation(result)
        (case / "result.json").write_text(json.dumps(result))
        checked = archive(cfg, base, case / "raw/latency.csv", case / "raw/lane_result.json",
                          run_id="run", mode="managed", pid=10, start_ticks=20,
                          graceful_finalization=True)
        assert checked["status"] == "invalid", name


def test_revalidation_binds_archive_and_lane_identity(tmp_path):
    config = _config(tmp_path); baseline = snapshot(config); _write_sources(tmp_path)
    artifact = archive(config, baseline, tmp_path / "raw/latency.csv", tmp_path / "raw/lane_result.json",
                       run_id="run", mode="managed", pid=10, start_ticks=20,
                       graceful_finalization=True)
    manifest = {"run_id": "run", "mode": "managed", "pid": 10,
                "pid_start_time_ticks": 20, "comparison_lane": "A", "start_time": 0}
    assert artifact["status"] == "supported" and revalidate(artifact, tmp_path, manifest)
    (tmp_path / "raw/latency.csv").write_text(_csv(values="1,2,1,1,2,3,4,6"))
    assert not revalidate(artifact, tmp_path, manifest)


def test_generic_managed_csv_lifecycle_is_formally_comparable(tmp_path):
    csv_path, result_path = tmp_path / "source.csv", tmp_path / "result.json"
    script = """import hashlib,json,os,signal,sys,time
csv_path,result_path=sys.argv[1:]
def stop(*_):
 data=b'label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\\nabc_ros2_4KB,3000,1,2,1,1,2,3,4,5\\n'
 open(csv_path,'wb').write(data)
 stat=open('/proc/self/stat').read(); ticks=int(stat[stat.rfind(')')+2:].split()[19])
 identity={'schema':'ibmw.abc.collector-managed.v1','run_id':os.environ['MW_PERF_RUN_ID'],'producer_pid':os.getpid(),'producer_start_time_ticks':ticks,'lane':'A','complete':True,'latency_csv_sha256':hashlib.sha256(data).hexdigest()}
 open(result_path,'w').write(json.dumps({'lane':'A','valid':True,'collector_managed':identity}))
 sys.exit(0)
signal.signal(signal.SIGTERM,stop)
while True: time.sleep(.01)
"""
    artifact = {"schema": "mw_perf.latency.csv.v1", "format": "LatencyStats CSV",
                "path": str(csv_path), "result_path": str(result_path), "required": True,
                "lane": "A", "expected_label": "abc_ros2_4KB", "expected_count": 3000}
    config = {"sut": {"mode": "managed", "command": [sys.executable, "-c", script,
                str(csv_path), str(result_path)]}, "run": {"sample_seconds": .05},
        "collectors": {"process": {"include_descendants": True, "interval_ms": 10},
                       "network": {"interfaces": ["lo"]}},
        "scenario_contract": {"scenario": "abc"},
        "comparison": {"schema": "mw_perf.comparison-metadata.v1", "contract": "c",
            "profile": "p", "workload": "w", "resource_scope": "r",
            "order": ["A", "B", "C"], "lane": "A", "block": "b", "sequence_index": 0},
        "artifacts": {"latency": artifact}}
    config_path = tmp_path / "config.json"; config_path.write_text(json.dumps(config))
    args = core.argparse.Namespace(config=str(config_path), adapter="generic", command=[],
                                   override_command=False, output=str(tmp_path / "runs"))
    assert core.run(args) == core.EXIT_OK
    run_dir = next((tmp_path / "runs").iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["measurement_kind"] == "latency_summary_csv"
    assert manifest["artifacts"]["latency"]["status"] == "supported"
    assert core.validate_dir(run_dir)["valid_for_comparison"] is True
    manifest["comparison_schema"] = "mw_perf.comparison-metadata.v0"
    (run_dir / "manifest.json").write_text(json.dumps(manifest))
    assert core.validate_dir(run_dir)["valid_for_comparison"] is False


def test_generic_managed_s_lane_lifecycle_is_formally_comparable(tmp_path):
    csv_path, result_path = tmp_path / "source.csv", tmp_path / "result.json"
    script = """import hashlib,json,os,signal,sys,time
csv_path,result_path=sys.argv[1:]
def stop(*_):
 data=b'label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\\nshm_send_4KB,3000,1,2,1,1,2,3,4,5\\n'
 open(csv_path,'wb').write(data)
 stat=open('/proc/self/stat').read(); ticks=int(stat[stat.rfind(')')+2:].split()[19])
 identity={'schema':'ibmw.abc.collector-managed.v1','run_id':os.environ['MW_PERF_RUN_ID'],'producer_pid':os.getpid(),'producer_start_time_ticks':ticks,'lane':'S','complete':True,'latency_csv_sha256':hashlib.sha256(data).hexdigest()}
 open(result_path,'w').write(json.dumps({'lane':'S','valid':True,'collector_managed':identity}))
 sys.exit(0)
signal.signal(signal.SIGTERM,stop)
while True: time.sleep(.01)
"""
    artifact = {"schema": "mw_perf.latency.csv.v1", "format": "LatencyStats CSV",
                "path": str(csv_path), "result_path": str(result_path), "required": True,
                "lane": "S", "expected_label": "shm_send_4KB", "expected_count": 3000}
    config = {"sut": {"mode": "managed", "command": [sys.executable, "-c", script,
                str(csv_path), str(result_path)]}, "run": {"sample_seconds": .05},
        "collectors": {"process": {"include_descendants": True, "interval_ms": 10},
                       "network": {"interfaces": ["lo"]}},
        "scenario_contract": {"scenario": "abc"},
        "comparison": {"schema": "mw_perf.comparison-metadata.v2", "contract": "c",
            "profile": "p", "workload": "w", "resource_scope": "r",
            "order": ["A", "B", "S"], "lane": "S", "block": "b", "sequence_index": 2},
        "artifacts": {"latency": artifact}}
    config_path = tmp_path / "config.json"; config_path.write_text(json.dumps(config))
    args = core.argparse.Namespace(config=str(config_path), adapter="generic", command=[],
                                   override_command=False, output=str(tmp_path / "runs"))
    assert core.run(args) == core.EXIT_OK
    run_dir = next((tmp_path / "runs").iterdir())
    manifest = json.loads((run_dir / "manifest.json").read_text())
    assert manifest["comparison_lane"] == "S"
    assert manifest["comparison_schema"] == "mw_perf.comparison-metadata.v2"
    assert manifest["artifacts"]["latency"]["provenance"]["lane"] == "S"
    validation = core.validate_dir(run_dir)
    assert validation["valid_for_comparison"] is True
    assert validation["schema_audit"]["schemas"]["result"]["schema_file"] == "schemas/result.schema.v2.json"
    manifest["comparison_lane"] = "A"
    (run_dir / "manifest.json").write_text(json.dumps(manifest))
    assert core.validate_dir(run_dir)["valid_for_comparison"] is False
