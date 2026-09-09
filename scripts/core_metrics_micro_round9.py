#!/usr/bin/env python3
"""Diagnostic Reporter micro-control projection; not an E2E performance gate."""
import argparse
import itertools
import json
import math
import os
import random
import statistics
import subprocess
from pathlib import Path

SEED = 190902971
BOOTSTRAPS = 100000
LANES = ("empty", "null", "noop_abi", "atomic")
DELTAS = {
    "null_empty": ("null", "empty"),
    "noop_null": ("noop_abi", "null"),
    "atomic_noop": ("atomic", "noop_abi"),
    "atomic_null": ("atomic", "null"),
    "atomic_empty": ("atomic", "empty"),
}
ALLOWANCES = {
    "off_out": ("null_empty", "out", 0.02),
    "counters_off": ("atomic_null", "off", 0.05),
    "counters_out": ("atomic_empty", "out", 0.07),
}


class InvalidRun(RuntimeError):
    pass


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def registered_orders():
    orders = list(itertools.permutations(LANES)) * 2
    random.Random(SEED).shuffle(orders)
    return orders


def validate_orders(orders):
    expected = set(itertools.permutations(LANES))
    tuples = [tuple(order) for order in orders]
    return len(tuples) == 48 and set(tuples) == expected and all(
        tuples.count(order) == 2 for order in expected)


def percentile(values, quantile):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1,
                       max(0, math.ceil(quantile * len(ordered)) - 1))]


def bootstrap_mean(values, seed=SEED, draws=BOOTSTRAPS):
    if not values:
        raise ValueError("cannot bootstrap an empty sample")
    rng = random.Random(seed)
    samples = [statistics.mean(rng.choice(values) for _ in values)
               for _ in range(draws)]
    return {"mean": statistics.mean(values),
            "lower": percentile(samples, 0.025),
            "upper": percentile(samples, 0.975),
            "draws": draws}


def parse_benchmark_output(stdout, expected_order):
    lines = [line for line in stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise InvalidRun("benchmark must emit exactly one JSON object")
    try:
        value = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise InvalidRun(f"invalid benchmark JSON: {error}") from error
    if not isinstance(value, dict) or value.get("order") != list(expected_order):
        raise InvalidRun("benchmark order does not match the registered block")
    if value.get("conservation_valid") is not True:
        raise InvalidRun("benchmark reported invalid conservation")
    if not isinstance(value.get("iterations"), int) or value["iterations"] <= 0:
        raise InvalidRun("invalid measured iteration count")
    if not isinstance(value.get("warmup"), int) or value["warmup"] < 0:
        raise InvalidRun("invalid warmup count")
    lanes = value.get("lanes")
    if not isinstance(lanes, dict) or set(lanes) != set(LANES):
        raise InvalidRun("benchmark lane set is invalid")
    for name in LANES:
        lane = lanes[name]
        if (not isinstance(lane, dict) or
                not isinstance(lane.get("elapsed_ns"), int) or
                lane["elapsed_ns"] < 0 or
                not isinstance(lane.get("ns_per_message"), (int, float)) or
                isinstance(lane["ns_per_message"], bool) or
                not math.isfinite(lane["ns_per_message"])):
            raise InvalidRun(f"invalid lane result: {name}")
    atomic = value.get("atomic")
    if (not isinstance(atomic, dict) or
            atomic.get("expected_count") != atomic.get("observed_count") or
            not isinstance(atomic.get("expected_count"), int)):
        raise InvalidRun("invalid atomic conservation fields")
    return value


def parse_atomic_artifact(path, expected_total):
    values = {}
    try:
        lines = path.read_text().splitlines()
    except OSError as error:
        raise InvalidRun(f"missing metrics artifact: {path}") from error
    for line in lines:
        try:
            item = json.loads(line)
        except json.JSONDecodeError as error:
            raise InvalidRun(f"invalid metrics JSON: {error}") from error
        if item.get("kind") == "reporter_micro_control":
            metric = item.get("metric")
            if metric in values:
                raise InvalidRun(f"duplicate atomic metric: {metric}")
            values[metric] = item.get("value")
    expected_names = {f"micro_atomic_{index}" for index in range(3)}
    if set(values) != expected_names or any(
            not isinstance(value, int) or value < 0 for value in values.values()):
        raise InvalidRun("atomic metrics artifact has the wrong slot set")
    if sum(values.values()) != expected_total:
        raise InvalidRun("persisted atomic count does not match benchmark")
    return values


def paired_deltas(records):
    output = {name: [] for name in DELTAS}
    for record in records:
        lanes = record["lanes"]
        for name, (numerator, denominator) in DELTAS.items():
            output[name].append(lanes[numerator]["ns_per_message"] -
                                lanes[denominator]["ns_per_message"])
    return output


def classify(ci, budget):
    if ci["upper"] <= budget:
        return "PASS"
    if ci["lower"] > budget:
        return "FAIL"
    return "INCONCLUSIVE"


def project_ci(delta_ci, rate):
    scale = rate / 1_000_000_000
    return {key: delta_ci[key] * scale for key in ("mean", "lower", "upper")}


def load_cores(value, mode):
    try:
        cores = float(value)
    except ValueError:
        source = json.loads(Path(value).read_text())
        candidates = (
            source.get(f"{mode}_cores"),
            source.get("mode_means", {}).get(mode, {}).get("cpu_cores"),
            source.get(mode, {}).get("cpu_cores"),
        )
        cores = next((float(item) for item in candidates if item is not None),
                     math.nan)
    if not math.isfinite(cores) or cores <= 0:
        raise ValueError(f"invalid {mode} E2E core value")
    return cores


def summarize(records, rate, out_cores, off_cores):
    deltas = paired_deltas(records)
    results = {}
    cores = {"out": out_cores, "off": off_cores}
    for index, (name, values) in enumerate(deltas.items()):
        delta_ci = bootstrap_mean(values, SEED + index)
        projected = project_ci(delta_ci, rate)
        results[name] = {
            "delta_ns_per_message": delta_ci,
            "projected_cores": projected,
            "fractions": {
                mode: {key: projected[key] / baseline
                       for key in ("mean", "lower", "upper")}
                for mode, baseline in cores.items()
            },
        }
    allowances = {}
    for name, (delta_name, baseline, fraction) in ALLOWANCES.items():
        budget_cores = cores[baseline] * fraction
        projected = results[delta_name]["projected_cores"]
        allowances[name] = {
            "delta": delta_name,
            "baseline": baseline,
            "budget_fraction": fraction,
            "budget_cores": budget_cores,
            "verdict": classify(projected, budget_cores),
        }
    return results, allowances


def run_block(benchmark, output, cpu, block, order):
    directory = output / "blocks" / f"block-{block:02d}"
    metrics = directory / "metrics"
    metrics.mkdir(parents=True)
    env = os.environ.copy()
    env.update({"IBMW_CORE_METRICS": "counters",
                "IBMW_OBSERVABILITY_OUTPUT_DIR": str(metrics)})
    command = ["taskset", "-c", str(cpu), str(benchmark), "--controls",
               ",".join(order)]
    completed = subprocess.run(command, cwd=benchmark.parent, env=env, text=True,
                               capture_output=True)
    (directory / "stdout.txt").write_text(completed.stdout)
    (directory / "stderr.txt").write_text(completed.stderr)
    if completed.returncode != 0:
        raise InvalidRun(f"block {block} exited {completed.returncode}")
    record = parse_benchmark_output(completed.stdout, order)
    persisted = parse_atomic_artifact(
        metrics / "metrics.jsonl", record["atomic"]["expected_count"])
    record.update(block=block, persisted_atomic=values_sorted(persisted))
    return record


def values_sorted(values):
    return {key: values[key] for key in sorted(values)}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run the round9 diagnostic Reporter micro-control projection")
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=27)
    parser.add_argument("--rate", type=float, default=448000)
    parser.add_argument("--out-cores", required=True,
                        help="out E2E cores, or a summary JSON path")
    parser.add_argument("--off-cores", required=True,
                        help="off E2E cores, or a summary JSON path")
    args = parser.parse_args(argv)
    benchmark = args.benchmark.resolve()
    output = args.output.resolve()
    if not benchmark.is_file() or not os.access(benchmark, os.X_OK):
        parser.error("--benchmark must be an executable file")
    if output.exists():
        parser.error("--output must be a fresh, non-existing directory")
    if not math.isfinite(args.rate) or args.rate <= 0:
        parser.error("--rate must be positive")
    try:
        out_cores = load_cores(args.out_cores, "out")
        off_cores = load_cores(args.off_cores, "off")
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as error:
        parser.error(str(error))
    orders = registered_orders()
    if not validate_orders(orders):
        raise RuntimeError("internal order registration is not balanced")
    output.mkdir(parents=True)
    plan = {
        "schema": "ibmw-core-metrics-micro-round9-plan-v1",
        "purpose": "diagnostic projection; not a substitute for E2E or latency evidence",
        "seed": SEED, "blocks": 48, "cpu": args.cpu, "rate": args.rate,
        "warmup": "fixed by benchmark", "iterations": "fixed by benchmark",
        "orders": [list(order) for order in orders], "bootstrap": BOOTSTRAPS,
        "e2e_cores": {"out": out_cores, "off": off_cores},
    }
    write_json(output / "plan.json", plan)
    records = []
    raw = output / "raw.jsonl"
    try:
        with raw.open("w") as stream:
            for block, order in enumerate(orders):
                record = run_block(benchmark, output, args.cpu, block, order)
                records.append(record)
                stream.write(json.dumps(record, sort_keys=True) + "\n")
                stream.flush()
        if not validate_orders([record["order"] for record in records]):
            raise InvalidRun("captured orders are not exactly balanced")
        deltas, allowances = summarize(records, args.rate, out_cores, off_cores)
        summary = {
            "schema": "ibmw-core-metrics-micro-round9-summary-v1",
            "purpose": "diagnostic/projection only; not a substitute for E2E or latency evidence",
            "blocks": len(records), "conservation_valid": True,
            "rate": args.rate, "e2e_cores": {"out": out_cores, "off": off_cores},
            "deltas": deltas, "allowances": allowances,
        }
        write_json(output / "summary.json", summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except (InvalidRun, OSError, ValueError, json.JSONDecodeError) as error:
        summary = {
            "schema": "ibmw-core-metrics-micro-round9-summary-v1",
            "purpose": "diagnostic/projection only; not a substitute for E2E or latency evidence",
            "conservation_valid": False, "error": str(error),
        }
        write_json(output / "summary.json", summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
