#!/usr/bin/env python3
"""Run the A/B/C (or A/B/S) matrix and optionally emit a comparison-plan bridge.

This runner does not own collector lifecycle or resource observation.  The
metadata it emits is therefore deliberately invalid for comparison until a
collector has replaced the validation artifact with independently validated
evidence.
"""
import argparse
import json
import pathlib
import os
import re
import subprocess
import sys


UNSUPPORTED = "unsupported: not declared by abc runner"
LANES = ("A", "B", "C", "S")
LANE_SEMANTIC_WARNINGS = {"C": "C is not DDS-equivalent and is reported separately",
                          "S": "S is not DDS-equivalent and is reported separately"}


def _metadata(value):
    return value if value is not None else {"status": "unsupported", "reason": UNSUPPORTED}


def _lanes(value):
    order = value.split(",")
    if any(lane not in LANES for lane in order) or len(set(order)) != len(order):
        raise argparse.ArgumentTypeError(
            f"expected unique comma-separated lanes from {','.join(LANES)}: {value}")
    return order


def _cached_transports(build):
    """Return the CMakeCache transport switches, or None when they are not known."""
    try:
        text = (build / "CMakeCache.txt").read_text(errors="replace")
    except OSError:
        return None
    switches = {match[1]: match[2].strip().upper() in ("ON", "TRUE", "1", "YES", "Y")
                for match in re.finditer(r"(?m)^IBMW_BUILD_(ICEORYX|SHM)_EXTENSION:[^=]*=(.*)$", text)}
    return switches if len(switches) == 2 else None


def _default_order(build):
    """A,B,C unless the build has shm and no iceoryx, where S replaces C."""
    switches = _cached_transports(build)
    if switches is None:
        switches = {"SHM": (build / "shm_send_publisher_app").is_file(),
                    "ICEORYX": (build / "iox_send_publisher_app").is_file()}
    return ["A", "B", "S"] if switches["SHM"] and not switches["ICEORYX"] else ["A", "B", "C"]


def _parser():
    parser = argparse.ArgumentParser()
    parser.add_argument("--blocks", type=int, default=5)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("abc-results"))
    parser.add_argument("--roudi", type=pathlib.Path, help="RouDi executable for lane C")
    parser.add_argument("--lanes", type=_lanes,
                        help="comma-separated lane order; default A,B,C, or A,B,S for a "
                             "build with shm_send and without iox_send")
    parser.add_argument("--comparison-plan", type=pathlib.Path,
                        help="also emit a formal comparison-plan.v1")
    parser.add_argument("--collector-managed", action="store_true",
                        help="run each lane under mw_perf_collect managed lifecycle")
    parser.add_argument("--contract", help="comparison contract identifier or JSON value")
    parser.add_argument("--profile", help="resource accounting profile or JSON value")
    parser.add_argument("--workload", help="workload identifier or JSON value")
    parser.add_argument("--resource-scope", dest="resource_scope",
                        help="resource scope or JSON value")
    parser.add_argument("--order-policy", default="fixed")
    parser.add_argument("--order-seed", type=int, default=0)
    return parser


def _declared(value):
    if value is None:
        return _metadata(None)
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value


def _write_bridge(work, lane, block_id, metadata, order, order_policy, order_seed, result):
    work.mkdir(parents=True, exist_ok=True)
    run_id = f"abc-{block_id}-{lane}"
    manifest = {
        "schema_version": 1,
        "tool_version": "abc-runner-bridge.v1",
        "run_id": run_id,
        "adapter": "generic",
        "mode": "attached",
        "run_status": "completed" if result.get("returncode") == 0
                      and result.get("valid") is True else "invalid",
        "metric_availability": "unsupported",
        "measurement_kind": "metadata",
        "comparison_lane": lane,
        "comparison_block_id": block_id,
        "contract": metadata["contract"],
        "profile": metadata["profile"],
        "workload": metadata["workload"],
        "resource_scope": metadata["resource_scope"],
        "order": order,
        "order_policy": order_policy,
        "order_seed": order_seed,
        "bridge_status": "unsupported",
        "bridge_reason": "collector managed lifecycle and resource/provenance evidence are not produced by run_matrix",
        "abc_result": result,
    }
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validation = {
        "validation_profile": "standard",
        "valid_for_collection": False,
        "valid_for_metric": False,
        "valid_for_comparison": False,
        "rules": [{"name": "collector_managed_lifecycle", "passed": False},
                  {"name": "resource_provenance", "passed": False}],
        "failure_codes": ["COLLECTOR_LIFECYCLE_UNSUPPORTED",
                          "RESOURCE_PROVENANCE_UNSUPPORTED"],
    }
    (work / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")


def _collector_environment():
    repository = pathlib.Path(__file__).resolve().parents[5]
    env = os.environ.copy()
    tools = str(repository / "tools")
    env["PYTHONPATH"] = tools + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    return env


def _managed_config(runner, build, source_work, lane, domain, roudi, metadata,
                    block_id, order, order_policy, order_seed, sequence_index):
    command = [sys.executable, str(runner), lane, "--build", str(build),
               "--work", str(source_work), "--domain", str(domain), "--collector-managed"]
    if roudi is not None:
        command.extend(["--roudi", str(roudi)])
    return {
        "schema_version": 1,
        "sut": {"mode": "managed", "command": command,
                "working_directory": str(pathlib.Path(__file__).resolve().parents[5])},
        "run": {"sample_seconds": 40, "stop_timeout_seconds": 10},
        "collectors": {"process": {"include_descendants": True, "interval_ms": 500},
                       "network": {"interfaces": ["lo"]}},
        "scenario_contract": {"scenario": "ibmw-abc-4kb-v1",
                              "message_count": 3000, "payload_bytes": 4096,
                              "hz": 100, "warmup_messages": 50},
        "comparison": {"schema": "mw_perf.comparison-metadata.v1", **metadata,
                       "order": order, "order_policy": order_policy,
                       "order_seed": order_seed, "lane": lane, "block": block_id,
                       "sequence_index": sequence_index},
        "artifacts": {"latency": {"schema": "mw_perf.latency.csv.v1",
            "format": "LatencyStats CSV", "path": str(source_work / "latency.csv"),
            "result_path": str(source_work / "result.json"), "required": True,
            "lane": lane, "expected_label": {"A": "abc_ros2_4KB", "B": "abc_dds_4KB",
                                                "C": "iox_send_4KB"}[lane],
            "expected_count": 3000}},
    }


def _run_managed_lane(base, config, lane):
    base.mkdir(parents=True, exist_ok=True)
    config_path = base / "collector-config.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    output = base / "collector-runs"
    before = set(output.iterdir()) if output.exists() else set()
    command = [sys.executable, "-m", "mw_perf_collect", "run", "--adapter", "generic",
               "--config", str(config_path), "--output", str(output)]
    try:
        completed = subprocess.run(command, timeout=150, env=_collector_environment())
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, {"lane": lane, "valid": False,
                      "validation_errors": [f"collector_run_error: {exc}"]}
    created = sorted(set(output.iterdir()) - before) if output.exists() else []
    if completed.returncode != 0 or len(created) != 1:
        return (created[0].resolve() if len(created) == 1 else None), {
            "lane": lane, "valid": False, "returncode": completed.returncode,
            "validation_errors": ["collector_run_failed_or_run_directory_ambiguous"]}
    run_dir = created[0].resolve()
    validate = [sys.executable, "-m", "mw_perf_collect", "validate", str(run_dir)]
    try:
        validation_rc = subprocess.run(validate, timeout=30, env=_collector_environment()).returncode
        lane_result = json.loads((pathlib.Path(config["artifacts"]["latency"]["result_path"])).read_text())
        validation = json.loads((run_dir / "validation.json").read_text())
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as exc:
        return run_dir, {"lane": lane, "valid": False,
                         "validation_errors": [f"collector_validation_error: {exc}"]}
    valid = validation_rc == 0 and validation.get("valid_for_comparison") is True and lane_result.get("valid") is True
    return run_dir, {**lane_result, "lane": lane, "valid": valid,
                     "returncode": completed.returncode, "validation_returncode": validation_rc,
                     "canonical_run_dir": str(run_dir),
                     "validation_errors": lane_result.get("validation_errors", []) + ([] if valid else ["collector_validation_failed"])}


def main(argv=None):
    a = _parser().parse_args(argv)
    if a.blocks < 1:
        _parser().error("--blocks must be >= 1")
    if a.order_seed < 0:
        _parser().error("--order-seed must be >= 0")
    order = a.lanes or _default_order(a.build)
    if (a.collector_managed or a.comparison_plan) and set(order) != set("ABC"):
        _parser().error("--collector-managed and --comparison-plan require exactly lanes A, B and C: "
                        "mw_perf_collect comparison schemas do not accept lane order " + ",".join(order))
    a.output.mkdir(parents=True, exist_ok=True)
    metadata = {name: _declared(value) for name, value in (
        ("contract", a.contract), ("profile", a.profile),
        ("workload", a.workload), ("resource_scope", a.resource_scope))}
    results = []
    runner = pathlib.Path(__file__).with_name("run_block.py")
    if a.collector_managed:
        missing = [name for name, value in (("contract", a.contract), ("profile", a.profile),
                   ("workload", a.workload), ("resource_scope", a.resource_scope)) if value is None]
        if missing:
            _parser().error("--collector-managed requires " + ", ".join("--" + x.replace("_", "-") for x in missing))
    canonical_runs = {}
    for block in range(1, a.blocks + 1):
        block_id = f"abc-block-{block}"
        for offset, lane in enumerate(order):
            work = a.output / f"block-{block}" / lane
            domain = 80 + block * max(3, len(order)) + offset
            if a.collector_managed:
                source_work = work / "lane-output"
                config = _managed_config(runner, a.build.resolve(), source_work, lane, domain,
                    a.roudi, metadata, block_id, order, a.order_policy, a.order_seed, offset)
                canonical, result = _run_managed_lane(work, config, lane)
                result.update(block=block, comparison_block_id=block_id)
                if canonical is not None:
                    canonical_runs[(block, lane)] = canonical
                results.append(result)
                continue
            command = [sys.executable, str(runner), lane, "--build", str(a.build),
                       "--work", str(work), "--domain", str(domain)]
            if a.roudi is not None:
                command.extend(["--roudi", str(a.roudi)])
            try:
                rc = subprocess.run(command, timeout=120).returncode
            except subprocess.TimeoutExpired:
                rc = 1
                result = {"lane": lane, "valid": False,
                          "validation_errors": ["runner_timeout_expired"]}
            except OSError as e:
                rc = 1
                result = {"lane": lane, "valid": False,
                          "validation_errors": [f"runner_start_error: {e}"]}
            else:
                try:
                    result = json.loads((work / "result.json").read_text())
                except (OSError, json.JSONDecodeError):
                    result = {"lane": lane, "valid": False,
                              "validation_errors": ["result.json_missing_or_unparseable"]}
            result.update(block=block, comparison_block_id=block_id, returncode=rc)
            _write_bridge(work, lane, block_id, metadata, order, a.order_policy,
                          a.order_seed, result)
            results.append(result)
    valid_runs = sum(r.get("valid") is True and r.get("returncode") == 0 for r in results)
    summary = {"blocks": a.blocks, "valid": bool(results) and valid_runs == len(results),
               "valid_runs": valid_runs, "invalid_runs": len(results) - valid_runs,
               "runs": results}
    semantic_warnings = [LANE_SEMANTIC_WARNINGS[lane] for lane in order if lane in LANE_SEMANTIC_WARNINGS]
    if semantic_warnings:
        summary["semantic_warning"] = "; ".join(semantic_warnings)
    (a.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    plan_path = a.comparison_plan or (a.output / "comparison-plan.json" if a.collector_managed else None)
    if plan_path:
        output_root = a.output.resolve()
        blocks = [{"block_id": f"abc-block-{block}", "runs": [
            {"lane": lane, "path": str(canonical_runs.get((block, lane),
                output_root / f"block-{block}" / lane))}
            for lane in order]} for block in range(1, a.blocks + 1)]
        plan = {"schema": "mw_perf.comparison-plan.v1", "schema_version": 1,
                "contract": metadata["contract"], "profile": metadata["profile"],
                "workload": metadata["workload"], "resource_scope": metadata["resource_scope"],
                "order": order, "order_policy": a.order_policy,
                "order_seed": a.order_seed, "blocks": blocks}
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text(json.dumps(plan, indent=2) + "\n")
    return 0 if summary["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
