#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Unit tests of perf_env.py: python3 -m unittest selfdev/shm/scripts/test_perf_env.py"""
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_env  # noqa: E402

NCPU = 20
WATCH = list(range(12, 20))


def snapshot(uptime, busy, idle, procs=(), freq=None, self_pid=999, end=True):
    """busy/idle: per-CPU cumulative jiffies (lists of NCPU)."""
    lines = ["PERF_ENV v1", f"self {self_pid}", f"uptime {uptime:.2f} 0.00", "epoch 1000"]
    lines.append(f"stat cpu  {sum(busy)} 0 0 {sum(idle)} 0 0 0 0 0 0")
    for c in range(NCPU):
        # busy split into user and system, idle into idle and iowait
        lines.append(f"stat cpu{c} {busy[c] - busy[c] // 4} 0 {busy[c] // 4} {idle[c] - idle[c] // 10} {idle[c] // 10} 0 0 0 0 0")
    for c in range(NCPU):
        lines.append(f"freq {c} {(freq or {}).get(c, 2316000)}")
    lines.append("ps   PID  PPID PSR     TIME ARGS")
    lines.append("ps     1     0   4 00:07:24 init --second-stage")
    for pid, ppid, t, args in procs:
        lines.append(f"ps {pid:5d} {ppid:5d}   3 {t} {args}")
    if end:
        lines.append("end")
    return "\n".join(lines) + "\n"


class Machine:
    """Cumulative /proc/stat counters at HZ=100."""

    def __init__(self):
        self.t = 1000.0
        self.busy = [50000] * NCPU
        self.idle = [900000] * NCPU

    def advance(self, seconds, load):
        """load: {cpu: busy fraction}; other CPUs idle."""
        self.t += seconds
        for c in range(NCPU):
            total = int(round(seconds * 100))
            b = int(round(total * load.get(c, 0.0)))
            self.busy[c] += b
            self.idle[c] += total - b

    def snap(self, **kw):
        return perf_env.parse_snapshot(snapshot(self.t, self.busy, self.idle, **kw))


def args(**kw):
    ns = dict(foreign=perf_env.DEFAULT_FOREIGN, max_foreign=perf_env.DEFAULT_MAX_FOREIGN,
              max_other=perf_env.DEFAULT_MAX_OTHER,
              quiet_cpu_busy=perf_env.DEFAULT_QUIET_CPU_BUSY, quiet_min_window=perf_env.DEFAULT_QUIET_MIN_WINDOW)
    ns.update(kw)
    return type("Args", (), ns)


class ParseTest(unittest.TestCase):
    def test_cpus(self):
        self.assertEqual(perf_env.parse_cpus("12-19"), WATCH)
        self.assertEqual(perf_env.parse_cpus("12,13,14,15,16,17,18,19"), WATCH)
        self.assertEqual(perf_env.parse_cpus("3 1,2-2"), [1, 2, 3])
        for bad in ("", "a", "5-3", "1;2"):
            with self.assertRaises(ValueError):
                perf_env.parse_cpus(bad)
        self.assertEqual(perf_env.fmt_cpus([1, 2, 3, 5, 7, 8]), "1-3,5,7-8")

    def test_cputime(self):
        self.assertEqual(perf_env.parse_cputime("00:07:24"), 444)
        self.assertEqual(perf_env.parse_cputime("1-02:00:01"), 86400 + 7201)
        self.assertEqual(perf_env.parse_cputime("05:40"), 340)

    def test_snapshot_errors(self):
        m = Machine()
        good = snapshot(m.t, m.busy, m.idle)
        with self.assertRaises(perf_env.SnapshotError):
            perf_env.parse_snapshot(good.replace("PERF_ENV v1", "garbage"))
        with self.assertRaises(perf_env.SnapshotError):
            perf_env.parse_snapshot(snapshot(m.t, m.busy, m.idle, end=False))
        # an empty or cut process list (ps failed) would hide foreign benchmarks
        no_ps = "\n".join(x for x in good.splitlines() if not x.startswith("ps ")) + "\n"
        with self.assertRaisesRegex(perf_env.SnapshotError, "without pid 1"):
            perf_env.parse_snapshot(no_ps)
        with self.assertRaisesRegex(perf_env.SnapshotError, "without pid 1"):
            perf_env.parse_snapshot(good.replace("ps     1     0   4 00:07:24 init --second-stage\n", ""))
        # a short or garbled /proc/stat line
        with self.assertRaisesRegex(perf_env.SnapshotError, "bad /proc/stat line"):
            perf_env.parse_snapshot(good.replace("stat cpu12 ", "stat cpu12 7 7 7\nstat cpu99 ", 1).replace(
                "stat cpu99 ", "stat cpu98 1 2 3 4\nstat cpu97 ", 1))
        with self.assertRaisesRegex(perf_env.SnapshotError, "bad /proc/stat line"):
            perf_env.parse_snapshot(good.replace("stat cpu3 ", "stat cpu3 x ", 1))
        s = perf_env.parse_snapshot("hdc noise\r\n" + good.replace("\n", "\r\n"))
        self.assertEqual(s["freq"][12], 2316000)
        self.assertEqual(len(s["stat"]), NCPU + 1)

    def test_time_output(self):
        self.assertEqual(perf_env.parse_time_output("RESULT x\nreal 10.5\nuser 7.25\nsys 0.75\nDEVICE_RC=0\n"),
                         {"real": 10.5, "user": 7.25, "sys": 0.75})
        self.assertEqual(perf_env.parse_time_output("real     0.20\nuser     0.00\nsys      0.00\n")["real"], 0.2)
        self.assertIsNone(perf_env.parse_time_output("real 1\nuser 1\n"))


class RoundTest(unittest.TestCase):
    def round(self, load, own, seconds=20.0, pre_procs=(), post_procs=(), quiet_load=None, **kw):
        m = Machine()
        q = m.snap()
        m.advance(2.5, quiet_load or {})
        pre = m.snap(procs=pre_procs)
        m.advance(seconds, load)
        post = m.snap(procs=post_procs)
        quiet = perf_env.quiet_check(q, pre, WATCH, args(**kw))
        return perf_env.round_analysis(pre, post, WATCH, args(**kw), own_cpu=own * seconds, wall=seconds - 0.2,
                                       quiet=quiet)

    def test_idle_with_own_load_is_clean(self):
        # the bench keeps CPUs 12..15 busy: 4 cores, all its own
        r = self.round({12: 1.0, 13: 1.0, 14: 1.0, 15: 1.0, 2: 0.5}, own=4.0)
        self.assertEqual(r["verdict"], "CLEAN", r["reasons"])
        self.assertAlmostEqual(r["watched_busy_cores"], 4.0, places=2)
        self.assertAlmostEqual(r["foreign_cores"], 0.0, places=2)
        self.assertAlmostEqual(r["system_busy_cores"], 4.5, places=2)

    def test_foreign_load_on_watched_cpus_is_disturbed(self):
        # mutation sanity: a forged snapshot with 0.5 foreign core on CPU 19
        r = self.round({12: 1.0, 13: 1.0, 19: 0.5}, own=2.0)
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertAlmostEqual(r["foreign_cores"], 0.5, places=2)
        self.assertTrue(any("foreign load" in x for x in r["reasons"]))

    def test_load_elsewhere(self):
        # idle-like system load on the other CPUs is clean
        r = self.round({12: 1.0, 0: 1.0, 1: 0.5}, own=1.0)
        self.assertEqual(r["verdict"], "CLEAN", r["reasons"])
        self.assertAlmostEqual(r["other_busy_cores"], 1.5, places=2)
        # a heavy load elsewhere (shared caches, memory, power budget) is not
        r = self.round({12: 1.0, 0: 1.0, 1: 1.0, 2: 1.0}, own=1.0)
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertAlmostEqual(r["system_busy_cores"], 4.0, places=2)
        self.assertAlmostEqual(r["other_busy_cores"], 3.0, places=2)
        self.assertTrue(any("load besides the bench 3.00" in x for x in r["reasons"]), r["reasons"])
        self.assertEqual(self.round({12: 1.0, 0: 1.0, 1: 1.0, 2: 1.0}, own=1.0, max_other=3.5)["verdict"], "CLEAN")

    def test_threshold_is_configurable(self):
        self.assertEqual(self.round({12: 1.0, 19: 0.15}, own=1.0)["verdict"], "DISTURBED")
        self.assertEqual(self.round({12: 1.0, 19: 0.15}, own=1.0, max_foreign=0.2)["verdict"], "CLEAN")

    def test_own_above_busy_is_clamped_and_disturbed(self):
        # the premise (the bench ran on the watched CPUs) is broken: no verdict on noise
        r = self.round({12: 0.5}, own=1.0)
        self.assertEqual(r["foreign_cores"], 0.0)
        self.assertAlmostEqual(r["foreign_cores_raw"], -0.5, places=2)
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertTrue(any("not pinned" in x for x in r["reasons"]), r["reasons"])
        # within the accounting granularity it stays clean
        self.assertEqual(self.round({12: 0.9}, own=1.0)["verdict"], "CLEAN")

    def test_wall_longer_than_window_is_disturbed(self):
        m = Machine()
        pre = m.snap()
        m.advance(10, {12: 1.0})
        r = perf_env.round_analysis(pre, m.snap(), WATCH, args(), own_cpu=10.0, wall=11.0)
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertTrue(any("longer than the snapshot window" in x for x in r["reasons"]), r["reasons"])

    def test_pin_failures_are_disturbed(self):
        m = Machine()
        pre = m.snap()
        m.advance(10, {12: 1.0})
        r = perf_env.round_analysis(pre, m.snap(), WATCH, args(), own_cpu=10.0, pin_failures=3)
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertEqual(r["pin_failures"], 3)
        self.assertEqual(perf_env.parse_pin_failures("RESULT x\nPIN_FAILURES=2\nPIN_FAILURES=0\nPIN_FAILURES=5\n"), 7)

    def test_foreign_benchmark_process_at_start_or_end(self):
        bench = (4000, 1, "00:00:02", "sh qb2.sh")
        q = (4001, 4000, "00:01:00", "q.fin2b --reps=3 --filter=queue")
        r = self.round({12: 1.0}, own=1.0, pre_procs=[bench])
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertEqual((r["foreign_bench_start"], r["foreign_bench_end"]), (1, 0))
        r = self.round({12: 1.0}, own=1.0, post_procs=[bench, q])
        self.assertEqual((r["foreign_bench_start"], r["foreign_bench_end"]), (0, 2))
        self.assertEqual(r["verdict"], "DISTURBED")

    def test_own_snippet_shell_and_grep_are_not_foreign(self):
        procs = [(999, 1, "00:00:00", "sh -c ... selfdev_st ..."), (1001, 999, "00:00:00", "ps -eo selfdev_st"),
                 (1002, 1, "00:00:00", "grep -E qb2.sh"), (1003, 1, "00:00:00", "ibmw_shm_ext_bench --cpus 12")]
        r = self.round({12: 1.0}, own=1.0, pre_procs=procs, post_procs=procs)
        self.assertEqual(r["verdict"], "CLEAN", r["reasons"])

    def test_pattern_is_configurable(self):
        procs = [(1003, 1, "00:00:00", "my_other_bench --x")]
        self.assertEqual(self.round({12: 1.0}, own=1.0, post_procs=procs)["verdict"], "CLEAN")
        self.assertEqual(self.round({12: 1.0}, own=1.0, post_procs=procs, foreign="other_bench")["verdict"],
                         "DISTURBED")

    def test_quiet_window(self):
        r = self.round({12: 1.0}, own=1.0, quiet_load={15: 0.3})
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertTrue(any("quiet: cpu15 busy 30%" in x for x in r["reasons"]), r["reasons"])
        m = Machine()
        a = m.snap()
        m.advance(1.0, {})
        res = perf_env.quiet_check(a, m.snap(), WATCH, args())
        self.assertFalse(res["quiet"])
        self.assertIn("quiet window 1.00s < 2s", res["reasons"][0])

    def test_unknown_own_cpu_is_disturbed(self):
        m = Machine()
        pre = m.snap()
        m.advance(10, {12: 1.0})
        r = perf_env.round_analysis(pre, m.snap(), WATCH, args())
        self.assertEqual(r["verdict"], "DISTURBED")
        self.assertIsNone(r["foreign_cores"])

    def test_freq_and_top(self):
        m = Machine()
        pre = m.snap(freq={12: 1315000}, procs=[(500, 1, "00:00:10", "render_service")])
        m.advance(10, {12: 1.0})
        post = m.snap(freq={19: 2316000}, procs=[(500, 1, "00:00:15", "render_service")])
        r = perf_env.round_analysis(pre, post, WATCH, args(), own_cpu=10.0)
        self.assertEqual((r["freq_min_khz"], r["freq_max_khz"]), (1315000, 2316000))
        # outside the load only: flagged, not a verdict
        self.assertTrue(r["freq_changed"])
        self.assertEqual(r["verdict"], "CLEAN", r["reasons"])
        self.assertTrue(any("cpufreq" in n for n in r["notes"]), r["notes"])
        self.assertEqual(r["top"][0]["pid"], 500)
        self.assertAlmostEqual(r["top"][0]["cores"], 0.5)


class BaselineTest(unittest.TestCase):
    def test_baseline(self):
        m = Machine()
        snaps = [m.snap()]
        for load in ({12: 0.02}, {13: 0.04}, {}):
            m.advance(2.0, load)
            snaps.append(m.snap())
        b = perf_env.baseline(snaps, WATCH, perf_env.DEFAULT_FOREIGN)
        self.assertEqual(b["pairs"], 3)
        self.assertAlmostEqual(b["per_cpu_busy_all"]["max"], 0.04)
        self.assertAlmostEqual(b["watched_busy_cores"]["max"], 0.04)
        self.assertEqual(b["pairs_with_foreign_bench"], 0)

    def test_baseline_leaves_out_foreign_windows(self):
        m = Machine()
        snaps = [m.snap()]
        for i in range(8):
            busy = i in (3, 4)                 # foreign load, snapshots 4 and 5 show it
            m.advance(2.0, {19: 0.9} if busy else {12: 0.02})
            snaps.append(m.snap(procs=[(777, 1, "00:00:01", "sh qb2.sh")] if busy else ()))
        b = perf_env.baseline(snaps, WATCH, perf_env.DEFAULT_FOREIGN, 2.0)
        self.assertEqual(b["pairs_with_foreign_bench"], 3)
        self.assertEqual(b["pairs"], 5)
        self.assertAlmostEqual(b["watched_busy_cores"]["max"], 0.02)
        self.assertEqual(b["round_window"]["windows"], 5)
        self.assertAlmostEqual(b["round_window"]["watched_busy_cores"]["max"], 0.02)

    def test_baseline_round_window(self):
        m = Machine()
        snaps = [m.snap()]
        # 1 s apart; one 1 s burst of 0.6 core on cpu19 and 3 cores elsewhere
        for i in range(12):
            m.advance(1.0, {19: 0.6, 0: 1.0, 1: 1.0, 2: 1.0} if i == 4 else {0: 0.5})
            snaps.append(m.snap())
        b = perf_env.baseline(snaps, WATCH, perf_env.DEFAULT_FOREIGN, 4.0)
        w = b["round_window"]
        self.assertEqual(w["windows"], 3)
        self.assertAlmostEqual(w["watched_busy_cores"]["max"], 0.15)
        self.assertAlmostEqual(w["system_busy_cores"]["max"], 0.15 + (3 * 0.5 + 3.0) / 4)
        self.assertEqual(w["watched_over_default_max_foreign"], round(1 / 3, 4))
        self.assertEqual(w["suggested"], {"max_foreign": 0.15, "max_other": 1.3})
        with self.assertRaises(perf_env.SnapshotError):
            perf_env.baseline(snaps, WATCH, perf_env.DEFAULT_FOREIGN, 100.0)


class CliTest(unittest.TestCase):
    def run_main(self, argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = perf_env.main(argv)
        return rc, out.getvalue()

    def test_exit_codes_and_json(self):
        m = Machine()
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "q.txt").write_text(snapshot(m.t, m.busy, m.idle))
            m.advance(3, {})
            (p / "pre.txt").write_text(snapshot(m.t, m.busy, m.idle))
            m.advance(10, {12: 1.0})
            (p / "post.txt").write_text(snapshot(m.t, m.busy, m.idle))
            (p / "time.txt").write_text("RESULT ...\nreal 9.8\nuser 9.5\nsys 0.5\n")
            m.advance(10, {12: 1.0, 13: 1.0})
            (p / "post2.txt").write_text(snapshot(m.t, m.busy, m.idle))
            rc, out = self.run_main(["quiet", "--cpus", "12-19", str(p / "q.txt"), str(p / "pre.txt")])
            self.assertEqual((rc, out.split()[:2]), (0, ["QUIET", "yes"]))
            common = ["round", "--cpus", "12-19", "--pre", str(p / "pre.txt"), "--quiet-pre", str(p / "q.txt")]
            rc, out = self.run_main(common + ["--post", str(p / "post.txt"), "--time", str(p / "time.txt"),
                                              "--json", str(p / "r.json")])
            self.assertEqual(rc, 0, out)
            self.assertTrue(out.startswith("VERDICT CLEAN"))
            self.assertEqual(json.loads((p / "r.json").read_text())["own_cpu_s"], 10.0)
            # same own time, but a second core was busy: 1 foreign core
            rc, out = self.run_main(common + ["--post", str(p / "post2.txt"), "--own-cpu", "10"])
            self.assertEqual(rc, 1)
            self.assertIn("VERDICT DISTURBED", out)
            rc, _ = self.run_main(["round", "--cpus", "12-19", "--pre", str(p / "missing.txt"),
                                   "--post", str(p / "post.txt"), "--own-cpu", "1"])
            self.assertEqual(rc, 2)
            rc, _ = self.run_main(["quiet", "--cpus", "x", str(p / "q.txt"), str(p / "pre.txt")])
            self.assertEqual(rc, 2)

    def test_snippet_runs_here(self):
        out = subprocess.run(["sh", "-c", perf_env.SNIPPET], capture_output=True, text=True, check=True).stdout
        s = perf_env.parse_snapshot(out)
        self.assertIn("cpu0", s["stat"])
        self.assertTrue(s["procs"])
        self.assertTrue(any(p["pid"] == s["self"] or p["ppid"] == s["self"] for p in s["procs"]))


if __name__ == "__main__":
    unittest.main()
