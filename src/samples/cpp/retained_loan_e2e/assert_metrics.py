#!/usr/bin/env python3
import json
import sys
from pathlib import Path

if len(sys.argv) != 3 or sys.argv[1] not in {"direct", "retained"}:
    raise SystemExit("usage: assert_metrics.py direct|retained metrics.jsonl")

records = [json.loads(line) for line in Path(sys.argv[2]).read_text().splitlines() if line]
mode = sys.argv[1]
kind = "direct_iceoryx_loan" if mode == "direct" else "retained_iceoryx_receive"
values = {r["metric"]: r["value"] for r in records if r.get("kind") == kind}

if mode == "direct":
    required = {"logical_attempt", "logical_success", "logical_fail", "internal_retry", "publish", "release"}
    assert required <= values.keys(), values
    assert values["logical_attempt"] == values["logical_success"] + values["logical_fail"]
    assert values["logical_success"] == values["publish"] + values["release"]
    assert values["logical_fail"] > 0 and values["internal_retry"] > 0
else:
    required = {"receive_delivered", "receive_released", "outstanding_current", "outstanding_peak", "limit_drop",
                "callback_entered", "callback_completed", "callback_threw_std", "callback_threw_nonstd"}
    assert required <= values.keys(), values
    assert values["receive_delivered"] == values["receive_released"]
    assert values["outstanding_current"] == 0
    assert values["outstanding_peak"] == 1
    assert values["limit_drop"] > 0
    assert values["callback_entered"] == values["callback_completed"]
    assert values["callback_threw_std"] == values["callback_threw_nonstd"] == 0

print(json.dumps({"kind": kind, "counters": values}, sort_keys=True))
