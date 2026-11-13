#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Proves what the Core Metrics arm of one ibmw_shm_ext_bench run counted.

Usage: shm_ext_bench_metrics_check.py out|off|counters <bench log> [<metrics.jsonl>]

  out       bench without Reporter V1: no METRICS_ lines, no metrics.jsonl
  off       Reporter V1 host with IBMW_CORE_METRICS unset: METRICS_MODE=off,
            METRICS_FINAL_SNAPSHOT=ok and no metrics.jsonl (nothing counted)
  counters  IBMW_CORE_METRICS=counters: METRICS_MODE=counters,
            METRICS_FINAL_SNAPSHOT=ok, and for each ctx_loan_publish RESULT
            (payload P, ops N) direct_shm_loan records of a topic b/ctx/P/...
            with publish >= N, logical_attempt >= N and
            logical_attempt == logical_success + logical_fail

ctx_loan_publish is the case on the direct loan path (RegisterSendType +
kCtxMetaThenPayload loans); RESULT ops exclude the discarded warm-up
repetition, which is counted too, hence >=. Prints one BENCH_METRICS_PASS or
BENCH_METRICS_FAIL line; exit 0 on pass, 1 on fail, 64 on usage errors.
"""
import json
import re
import sys
from pathlib import Path

RESULT = re.compile(r"^RESULT case=ctx_loan_publish payload=(\d+) threads=1 \S+ \S+ \S+ \S+ ops=(\d+) ")


def check(mode, log_path, metrics_path):
    log = Path(log_path).read_text(errors="replace").splitlines()
    modes = [x.split("=", 1)[1] for x in log if x.startswith("METRICS_MODE=")]
    finals = [x.split("=", 1)[1] for x in log if x.startswith("METRICS_FINAL_SNAPSHOT=")]
    has_file = metrics_path is not None and Path(metrics_path).is_file()
    if mode == "out":
        if modes or finals:
            return False, f"METRICS lines in a bench without Reporter V1: {modes} {finals}"
        return (not has_file), "no metrics" if not has_file else f"unexpected {metrics_path}"
    if modes != [mode]:
        return False, f"METRICS_MODE {modes}, expected [{mode!r}]"
    if finals != ["ok"]:
        return False, f"METRICS_FINAL_SNAPSHOT {finals}, expected ['ok']"
    if mode == "off":
        return (not has_file), "nothing written" if not has_file else "metrics.jsonl written with metrics off"
    if not has_file:
        return False, "no metrics.jsonl"
    counters = {}
    for line in Path(metrics_path).read_text().splitlines():
        if not line:
            continue
        r = json.loads(line)
        if r.get("kind") == "direct_shm_loan":
            counters.setdefault(r.get("topic"), {})[r.get("metric")] = r.get("value")
    cases = [(int(m[1]), int(m[2])) for m in map(RESULT.match, log) if m]
    if not cases:
        return False, "no ctx_loan_publish RESULT"
    parts = []
    for payload, ops in cases:
        topics = [t for t in counters if isinstance(t, str) and t.startswith(f"b/ctx/{payload}/")]
        if len(topics) != 1:
            return False, f"ctx {payload}B: direct_shm_loan topics {topics}"
        c = counters[topics[0]]
        attempt, success, failed, publish = (c.get(k) for k in ("logical_attempt", "logical_success",
                                                                "logical_fail", "publish"))
        if not all(type(v) is int for v in (attempt, success, failed, publish)):
            return False, f"ctx {payload}B: incomplete counters {c}"
        if publish < ops or attempt < ops or attempt != success + failed:
            return False, f"ctx {payload}B: counters {c} do not cover ops={ops}"
        parts.append(f"ctx{payload}:ops={ops},attempt={attempt},publish={publish}")
    return True, " ".join(parts)


def main(argv):
    if len(argv) not in (2, 3) or argv[0] not in ("out", "off", "counters"):
        print(__doc__, file=sys.stderr)
        return 64
    ok, detail = check(argv[0], argv[1], argv[2] if len(argv) == 3 else None)
    print(f"BENCH_METRICS_{'PASS' if ok else 'FAIL'} mode={argv[0]} {detail}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
