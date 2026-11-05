#!/usr/bin/env python3
"""shm counterpart of assert_metrics.py: the checks of assert_metrics.py on the
shm Reporter V1 kinds, plus exact direct counts from direct.shm.metrics.json,
retained lower bounds, and the expected driver label on every record of the kind.

--check-run adds the exact retained form of check_shm_metrics.sh, where the
subscriber exits after the first recovery callback: delivered == entered == 2
and limit_drop below LIMIT_DROP_MAX. Managed runs (subscriber alive until the
collector's TERM) deliver many more samples, so only the bounds apply there.
Failures raise SystemExit (not assert), so they still fail under python -O."""
import json
import sys
from pathlib import Path

USAGE = "usage: assert_shm_metrics.py [--driver NAME] [--check-run] direct|retained metrics.jsonl"
# Observed 23-24 drops per check run; the late-release race (the releaser held
# the loan until its 7 s timeout) gave 229. Anything near that is a regression.
LIMIT_DROP_MAX = 120


def fail(message):
    raise SystemExit(f"assert_shm_metrics: {message}")


args = sys.argv[1:]
driver = "shm"
check_run = False
while args and args[0].startswith("--"):
    if args[0] == "--driver" and len(args) >= 2 and args[1]:
        driver = args[1]
        args = args[2:]
    elif args[0] == "--check-run":
        check_run = True
        args = args[1:]
    else:
        raise SystemExit(USAGE)
if len(args) != 2 or args[0] not in {"direct", "retained"}:
    raise SystemExit(USAGE)

mode = args[0]
records = [json.loads(line) for line in Path(args[1]).read_text().splitlines() if line]
kind = "direct_shm_loan" if mode == "direct" else "retained_shm_receive"
of_kind = [r for r in records if r.get("kind") == kind]
if not of_kind:
    fail(f"no {kind} records")
drivers = sorted({r.get("driver") for r in of_kind}, key=str)
if drivers != [driver]:
    fail(f"{kind} driver labels {drivers}, expected [{driver!r}]")


def plain_count(value):
    # bool is an int subclass, and 3.0 == 3: accept only real non-negative ints.
    return type(value) is int and value >= 0


topics = {(r.get("topic"), r.get("type")) for r in of_kind}
if len(topics) != 1 or not all(isinstance(f, str) and f for pair in topics for f in pair):
    fail(f"{kind} records need one topic/type pair of non-empty strings, got {sorted(topics, key=str)}")
# The workloads write one final snapshot, one record per metric. Several
# snapshots in one file are accepted only as a cumulative series: distinct
# snapshot_seq values and, except for the outstanding_current gauge, values
# that never decrease. The latest snapshot is the one checked.
GAUGES = {"outstanding_current"}
series = {}
for r in of_kind:
    metric, value = r.get("metric"), r.get("value")
    if not isinstance(metric, str) or not plain_count(value):
        fail(f"{kind} record with metric {metric!r} has value {value!r}, expected a non-negative int")
    cumulative = r.get("cumulative")
    if not isinstance(cumulative, dict) or type(cumulative.get("value")) is not int:
        fail(f"{kind} {metric}: no integer cumulative.value in {cumulative!r}")
    if cumulative["value"] != value:
        fail(f"{kind} {metric}: value {value!r} != cumulative.value {cumulative['value']!r}")
    series.setdefault(metric, []).append(r)
values = {}
for metric, rs in series.items():
    if len(rs) > 1:
        seqs = [r.get("snapshot_seq") for r in rs]
        if not all(plain_count(s) for s in seqs) or len(set(seqs)) != len(seqs):
            fail(f"{kind} {metric}: {len(rs)} records without distinct snapshot_seq {seqs}: conflicting duplicates")
        rs = sorted(rs, key=lambda r: r["snapshot_seq"])
        if metric not in GAUGES and any(a["value"] > b["value"] for a, b in zip(rs, rs[1:])):
            fail(f"{kind} {metric}: cumulative values decrease across snapshots {[r['value'] for r in rs]}")
    values[metric] = rs[-1]["value"]


def check(condition, message):
    if not condition:
        fail(f"{kind}: {message}; counters {values}")


if mode == "direct":
    required = {"logical_attempt", "logical_success", "logical_fail", "internal_retry", "publish", "release"}
    check(required <= values.keys(), f"missing {sorted(required - values.keys())}")
    contract = json.loads((Path(__file__).resolve().parent / "direct.shm.metrics.json").read_text())
    workload = contract["scenario_contract"]["workload"]
    semantics = contract["scenario_contract"]["semantics"]
    expected = {
        "logical_attempt": workload["attempts"],
        "logical_fail": workload["terminal_failure_attempts"],
        "logical_success": workload["attempts"] - workload["terminal_failure_attempts"],
        "publish": semantics["publish_successes"],
        "release": semantics["release_successes"],
    }
    for metric, value in expected.items():
        check(values[metric] == value, f"{metric} {values[metric]} != {value} (direct.shm.metrics.json)")
    check(values["logical_attempt"] == values["logical_success"] + values["logical_fail"],
          "logical_attempt != logical_success + logical_fail")
    check(values["logical_success"] == values["publish"] + values["release"],
          "logical_success != publish + release")
    check(values["internal_retry"] > 0, "internal_retry not > 0")
else:
    required = {"receive_delivered", "receive_released", "outstanding_current", "outstanding_peak", "limit_drop",
                "callback_entered", "callback_completed", "callback_threw_std", "callback_threw_nonstd"}
    check(required <= values.keys(), f"missing {sorted(required - values.keys())}")
    check(values["receive_delivered"] == values["receive_released"], "receive_delivered != receive_released")
    check(values["receive_delivered"] >= 2, "receive_delivered < 2 (held sample plus recovery)")
    # A delivery whose release token cannot be allocated is released without a
    # callback, so entered may fall short of delivered but never exceed it.
    check(values["callback_entered"] <= values["receive_delivered"], "callback_entered > receive_delivered")
    check(values["outstanding_current"] == 0, "outstanding_current != 0")
    check(values["outstanding_peak"] == 1, "outstanding_peak != 1")
    check(values["limit_drop"] > 0, "limit_drop not > 0")
    check(values["callback_entered"] == values["callback_completed"], "callback_entered != callback_completed")
    check(values["callback_threw_std"] == 0 and values["callback_threw_nonstd"] == 0, "callback threw")
    if check_run:
        check(values["receive_delivered"] == 2, "check run: receive_delivered != 2")
        check(values["callback_entered"] == values["receive_delivered"], "check run: callback_entered != receive_delivered")
        check(values["limit_drop"] < LIMIT_DROP_MAX, f"check run: limit_drop not < {LIMIT_DROP_MAX} (late release)")

print(json.dumps({"kind": kind, "driver": driver, "counters": values}, sort_keys=True))
