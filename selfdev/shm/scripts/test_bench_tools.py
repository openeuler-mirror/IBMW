#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Unit tests of shm_ext_bench_summary.py (--clean-only, --min-clean, --planned) and
shm_ext_bench_metrics_check.py: python3 -m unittest selfdev/shm/scripts/test_bench_tools.py"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shm_ext_bench_metrics_check as mcheck  # noqa: E402
import shm_ext_bench_summary as summary  # noqa: E402


def result(case, payload, ns, ops=1000, threads=1):
    return (f"RESULT case={case} payload={payload} threads={threads} ns_per_op={ns} mops={1000 / ns:.3f} "
            f"rep_min_mops=1 rep_max_mops=1 ops={ops} fails=0 delivered=0")


def write_round(d, label, rnd, ns, verdict="CLEAN", pins=0):
    lines = [result("gate_pair", 0, ns), f"PIN_FAILURES={pins}", "real 1.0", "user 1.0", "sys 0.0", "DEVICE_RC=0"]
    (d / f"{label}.{rnd}.log").write_text("\n".join(lines) + "\n")
    if verdict:
        (d / f"{label}.{rnd}.env.json").write_text(json.dumps({"verdict": verdict, "reasons": ["r"]}))


def run_summary(*args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = summary.main(*args)
    return rc, out.getvalue()


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_clean_only_drops_disturbed_rounds(self):
        for r in range(1, 7):
            write_round(self.d, "a", r, 30.0)
            write_round(self.d, "b", r, 31.0 if r != 3 else 90.0, "DISTURBED" if r == 3 else "CLEAN")
        rc, out = run_summary(self.d, ["a", "b"], True)
        self.assertEqual(rc, 0, out)
        self.assertIn("b: 5 clean of 6 rounds (minimum 4)", out)
        self.assertIn("paired: 5 rounds clean in both a and b (minimum 4)", out)
        self.assertIn("31.0 [31.0..31.0]", out)       # round 3 (90 ns) left out
        self.assertIn("+1.0 [+1.0..+1.0]", out)
        self.assertIn("pin failures in the CLEAN rounds of a, b: 0", out)
        # without --clean-only every round counts, as before
        rc, out = run_summary(self.d, ["a", "b"])
        self.assertEqual(rc, 0)
        self.assertIn("90.0", out)
        self.assertIn("pin failures in all runs", out)

    def test_inconclusive_by_label_planned_and_pairs(self):
        for r in range(1, 7):
            write_round(self.d, "a", r, 30.0, "CLEAN" if r <= 4 else "DISTURBED")
            write_round(self.d, "b", r, 31.0, "CLEAN" if r >= 3 else "DISTURBED")
        # each label has 4 of 6 clean (minimum 4), but only rounds 3 and 4 are clean in both
        rc, out = run_summary(self.d, ["a", "b"], True)
        self.assertEqual(rc, 3, out)
        self.assertIn("paired: 2 rounds clean in both a and b (minimum 4)", out)
        self.assertIn("INCONCLUSIVE", out)
        rc, out = run_summary(self.d, ["a", "b"], True, 2)
        self.assertEqual(rc, 0, out)
        # 6 rounds found, 12 planned (the run stopped early): minimum 8
        rc, out = run_summary(self.d, ["a", "b"], True, None, 12)
        self.assertEqual(rc, 3)
        self.assertIn("a: 4 clean of 6 rounds, 12 planned (minimum 8)", out)

    def test_missing_verdict_is_not_clean_and_no_result_fails(self):
        for r in range(1, 4):
            write_round(self.d, "a", r, 30.0, None)
        rc, out = run_summary(self.d, None, True)
        self.assertEqual(rc, 3)
        self.assertIn("a.1 NONE: no verdict file", out)
        rc, _ = run_summary(self.d, None, False)
        self.assertEqual(rc, 0)

    def test_outliers_listed_not_removed(self):
        for r in range(1, 9):
            write_round(self.d, "a", r, 50.0 if r == 5 else 30.0 + 0.1 * r)
        rc, out = run_summary(self.d, None, True)
        self.assertEqual(rc, 0, out)
        self.assertIn("gate_pair 0B x1 a (median 30.5): round:value 5:50.0", out)
        self.assertIn("50.0", out.split("## summary")[1])  # still in the min..max range
        for r in range(1, 9):
            write_round(self.d, "a", r, 30.0 + 0.1 * r)
        self.assertTrue(run_summary(self.d, None, True)[1].rstrip().endswith("none"))


class MetricsCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.log = self.d / "b.log"
        self.jsonl = self.d / "metrics.jsonl"

    def tearDown(self):
        self.tmp.cleanup()

    def bench_log(self, mode="counters", final="ok", ops=(100, 200)):
        lines = [result("gate_pair", 0, 30.0)]
        if mode:
            lines.append(f"METRICS_MODE={mode}")
        lines += [result("ctx_loan_publish", 64, 650.0, ops[0]), result("ctx_loan_publish", 4096, 650.0, ops[1])]
        if final:
            lines.append(f"METRICS_FINAL_SNAPSHOT={final}")
        self.log.write_text("\n".join(lines) + "\n")

    def counters(self, rows):
        out = []
        for payload, c in rows.items():
            for k, v in c.items():
                out.append(json.dumps({"kind": "direct_shm_loan", "topic": f"b/ctx/{payload}/0", "metric": k, "value": v}))
        self.jsonl.write_text("\n".join(out) + "\n")

    def check(self, mode, with_file=True):
        return mcheck.check(mode, self.log, self.jsonl if with_file else None)

    def good(self, ops, extra=10):
        n = ops + extra
        return {"logical_attempt": n, "logical_success": n - 1, "logical_fail": 1, "publish": n}

    def test_counters(self):
        self.bench_log()
        self.counters({64: self.good(100), 4096: self.good(200)})
        ok, detail = self.check("counters")
        self.assertTrue(ok, detail)
        self.assertEqual(detail, "ctx64:ops=100,attempt=110,publish=110 ctx4096:ops=200,attempt=210,publish=210")
        # fewer publishes than RESULT ops: not counted
        self.counters({64: self.good(100), 4096: {**self.good(200), "publish": 150}})
        self.assertFalse(self.check("counters")[0])
        # attempt != success + fail
        self.counters({64: {**self.good(100), "logical_fail": 3}, 4096: self.good(200)})
        self.assertFalse(self.check("counters")[0])
        # a payload without its topic
        self.counters({64: self.good(100)})
        self.assertIn("topics []", self.check("counters")[1])
        # no file, wrong mode, no final snapshot
        self.assertEqual(self.check("counters", False), (False, "no metrics.jsonl"))
        self.bench_log(mode="off")
        self.assertFalse(self.check("counters")[0])
        self.bench_log(final=None)
        self.assertFalse(self.check("counters")[0])

    def test_off_and_out(self):
        self.bench_log(mode="off")
        self.assertEqual(self.check("off", False), (True, "nothing written"))
        self.jsonl.write_text("{}\n")
        self.assertFalse(self.check("off")[0])
        self.bench_log(mode=None, final=None)
        self.assertEqual(self.check("out", False), (True, "no metrics"))
        self.assertFalse(self.check("out")[0])
        self.bench_log(mode="counters")
        self.assertFalse(self.check("out", False)[0])

    def test_cli(self):
        self.bench_log(mode="off")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(mcheck.main(["off", str(self.log)]), 0)
            self.assertEqual(mcheck.main(["counters", str(self.log)]), 1)
        self.assertIn("BENCH_METRICS_PASS mode=off", out.getvalue())
        self.assertIn("BENCH_METRICS_FAIL mode=counters", out.getvalue())
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(mcheck.main(["bogus", str(self.log)]), 64)


if __name__ == "__main__":
    unittest.main()
