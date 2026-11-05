#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Interference monitor for performance rounds on a shared machine (OHOS device or Linux host).

A snapshot is the text the POSIX-sh snippet prints (`perf_env.py snippet`): the
uptime, the cpu lines of /proc/stat, the cpufreq of every CPU and the process
list with cumulative CPU time. Two snapshots around a round give, for the
watched CPUs (the ones the bench is pinned to with taskset):

  busy      cores busy on the watched CPUs (sum of the per-CPU busy fractions)
  own       CPU seconds of the bench (user + sys, from `time -p`) / window
  foreign   busy - own, clamped at 0 (the raw value is reported too)
  system    cores busy on all CPUs
  other     system - own: everything but the bench on the whole machine (a
            heavy load elsewhere shares caches, memory bandwidth and the
            power budget with the bench)
  foreign benchmark processes (FOREIGN pattern) in the first and the last snapshot
  freq      min/max scaling_cur_freq of the watched CPUs in both snapshots
  top       processes with the most CPU time between the snapshots

/proc/loadavg is not used: on the HongMeng device it stays near 21 when the
machine is idle.

Subcommands:
  snippet                       print the snippet (run it with sh; the device
                                and a Linux host both have the needed tools)
  quiet  --cpus L A B           pre-round check over the window A -> B; exit 0 quiet, 1 not
  round  --cpus L --pre P --post Q [--quiet-pre A] [--time T | --own-cpu S]
                                verdict of a round, exit 0 CLEAN, 1 DISTURBED
  baseline --cpus L [--window W] S1 S2 ...
                                idle statistics over consecutive snapshot pairs and,
                                with --window, over windows of about W seconds (the
                                length of a round) with suggested thresholds
Errors (unreadable or malformed snapshots, bad arguments) exit 2.
"""
import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

# The guard pattern of the device plan: the other user's benchmark drivers.
DEFAULT_FOREIGN = r"qb\.sh|qb2\.sh|q\.[a-z0-9]+ --|selfdev_st|ft[0-9]+\.sh"
# Defaults, see selfdev/shm/docs/测试文档.md §6.18 for the idle baseline they rest on.
DEFAULT_MAX_FOREIGN = 0.10       # cores on the watched set during a round
DEFAULT_MAX_OTHER = 2.00         # cores busy on the whole machine besides the bench
DEFAULT_QUIET_CPU_BUSY = 0.10    # per watched CPU during the quiet window
DEFAULT_QUIET_MIN_WINDOW = 2.0   # seconds

# Keep it POSIX sh and toybox-compatible: no arrays, no `local`, no GNU options.
SNIPPET = r"""echo "PERF_ENV v1"
echo "self $$"
echo "uptime $(cat /proc/uptime)"
echo "epoch $(date +%s)"
grep '^cpu' /proc/stat | sed 's/^/stat /'
for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
  [ -r "$f" ] || continue
  c=${f#/sys/devices/system/cpu/cpu}
  echo "freq ${c%%/*} $(cat "$f" 2>/dev/null)"
done
ps -eo pid,ppid,psr,time,args 2>/dev/null | sed 's/^/ps /'
echo "end"
"""


class SnapshotError(Exception):
    pass


def parse_cpus(text):
    """'12-19' or '12,13,14' or '12 13' or a mix -> sorted list of ints."""
    cpus = set()
    for part in re.split(r"[,\s]+", text.strip()):
        if not part:
            continue
        m = re.fullmatch(r"(\d+)(?:-(\d+))?", part)
        if not m:
            raise ValueError(f"bad CPU list {text!r}")
        lo, hi = int(m[1]), int(m[2] or m[1])
        if hi < lo:
            raise ValueError(f"bad CPU range {part!r}")
        cpus.update(range(lo, hi + 1))
    if not cpus:
        raise ValueError("empty CPU list")
    return sorted(cpus)


def parse_cputime(text):
    """ps TIME: [DD-]HH:MM:SS or MM:SS (toybox and procps) -> seconds."""
    days = 0
    if "-" in text:
        d, text = text.split("-", 1)
        days = int(d)
    parts = [float(x) for x in text.split(":")]
    sec = 0.0
    for p in parts:
        sec = sec * 60 + p
    return days * 86400 + sec


def parse_snapshot(text, name="snapshot"):
    snap = {"uptime": None, "epoch": None, "self": None, "stat": {}, "freq": {}, "procs": [], "complete": False}
    header = False
    ps_header = None
    for raw in text.splitlines():
        line = raw.rstrip("\r")
        if line == "PERF_ENV v1":
            header = True
            continue
        if not header:
            continue
        key, _, rest = line.partition(" ")
        if key == "self":
            snap["self"] = int(rest)
        elif key == "uptime":
            snap["uptime"] = float(rest.split()[0])
        elif key == "epoch":
            snap["epoch"] = int(rest) if rest.strip().isdigit() else None
        elif key == "stat":
            f = rest.split()
            if f and f[0].startswith("cpu"):
                # user nice system idle at least; iowait and the rest are optional
                if len(f) < 5 or not all(x.isdigit() for x in f[1:]):
                    raise SnapshotError(f"{name}: bad /proc/stat line {rest!r}")
                snap["stat"][f[0]] = [int(x) for x in f[1:]]
        elif key == "freq":
            f = rest.split()
            if len(f) == 2 and f[0].isdigit() and f[1].isdigit():
                snap["freq"][int(f[0])] = int(f[1])
        elif key == "ps":
            f = rest.split(None, 4)
            if ps_header is None:
                ps_header = f
                continue
            if len(f) < 4 or not f[0].isdigit():
                continue
            try:
                snap["procs"].append({"pid": int(f[0]), "ppid": int(f[1]), "psr": int(f[2]) if f[2].isdigit() else -1,
                                      "time": parse_cputime(f[3]), "args": f[4] if len(f) > 4 else ""})
            except ValueError:
                continue
        elif key == "end":
            snap["complete"] = True
    if not header:
        raise SnapshotError(f"{name}: no 'PERF_ENV v1' header")
    if not snap["complete"]:
        raise SnapshotError(f"{name}: truncated (no 'end' line)")
    if snap["uptime"] is None or "cpu" not in snap["stat"]:
        raise SnapshotError(f"{name}: no uptime or no /proc/stat cpu line")
    # An empty or cut process list would hide foreign benchmarks.
    if not any(p["pid"] == 1 for p in snap["procs"]):
        raise SnapshotError(f"{name}: process list without pid 1 ({len(snap['procs'])} processes)")
    return snap


def load_snapshot(path):
    try:
        return parse_snapshot(Path(path).read_text(errors="replace"), str(path))
    except OSError as e:
        raise SnapshotError(f"{path}: {e.strerror}") from e


def cpu_busy(pre, post, cpu):
    """Busy fraction of cpu<N> (or 'cpu' for the total, as a fraction of all CPUs)."""
    a, b = pre["stat"].get(cpu), post["stat"].get(cpu)
    if a is None or b is None:
        return None
    n = min(len(a), len(b), 8)  # user nice system idle iowait irq softirq steal (guest is in user)
    d = [b[i] - a[i] for i in range(n)]
    total = sum(d)
    if total <= 0 or any(x < 0 for x in d):
        return None
    idle = d[3] + (d[4] if n > 4 else 0)
    return (total - idle) / total


def foreign_procs(snap, pattern):
    rx = re.compile(pattern)
    me = snap["self"]
    return [p for p in snap["procs"]
            if rx.search(p["args"]) and p["pid"] != me and p["ppid"] != me and "grep" not in p["args"]]


def top_procs(pre, post, window, count=8):
    before = {(p["pid"], p["args"]): p["time"] for p in pre["procs"]}
    rows = []
    for p in post["procs"]:
        key = (p["pid"], p["args"])
        if key in before:
            delta = p["time"] - before[key]
            if delta > 0:
                rows.append({"pid": p["pid"], "cores": round(delta / window, 3), "psr": p["psr"], "args": p["args"][:120]})
    rows.sort(key=lambda r: -r["cores"])
    return rows[:count]


def parse_pin_failures(text):
    """Sum of the PIN_FAILURES=<n> lines of a bench log (threads that could not be pinned)."""
    return sum(int(m[1]) for m in re.finditer(r"^PIN_FAILURES=(\d+)\s*$", text, re.M))


def parse_time_output(text):
    """Last real/user/sys of `time -p` (toybox or GNU) in a log -> dict or None."""
    found = {}
    for line in text.splitlines():
        m = re.fullmatch(r"\s*(real|user|sys)\s+([0-9]+(?:\.[0-9]+)?)\s*", line.rstrip("\r"))
        if m:
            found[m[1]] = float(m[2])
    return found if {"real", "user", "sys"} <= found.keys() else None


def window_stats(pre, post, cpus):
    window = post["uptime"] - pre["uptime"]
    per_cpu, missing = {}, []
    for c in cpus:
        v = cpu_busy(pre, post, f"cpu{c}")
        if v is None:
            missing.append(c)
        else:
            per_cpu[c] = v
    all_cpus = sorted(int(k[3:]) for k in post["stat"] if k[3:].isdigit())
    system = sum(v for v in (cpu_busy(pre, post, f"cpu{c}") for c in all_cpus) if v is not None)
    return window, per_cpu, missing, system


def quiet_check(pre, post, cpus, args):
    window, per_cpu, missing, system = window_stats(pre, post, cpus)
    reasons = []
    if window < args.quiet_min_window:
        reasons.append(f"quiet window {window:.2f}s < {args.quiet_min_window:g}s")
    if missing:
        reasons.append(f"quiet: no /proc/stat data for cpu {','.join(map(str, missing))}")
    for c, v in per_cpu.items():
        if v > args.quiet_cpu_busy:
            reasons.append(f"quiet: cpu{c} busy {100 * v:.0f}% > {100 * args.quiet_cpu_busy:.0f}%")
    fp = foreign_procs(post, args.foreign)
    if fp:
        reasons.append(f"quiet: {len(fp)} foreign benchmark process(es): " + "; ".join(p["args"][:60] for p in fp[:3]))
    return {"window_s": round(window, 3), "per_cpu_busy": {str(c): round(v, 4) for c, v in per_cpu.items()},
            "watched_busy_cores": round(sum(per_cpu.values()), 4), "system_busy_cores": round(system, 3),
            "quiet": not reasons, "reasons": reasons}


def round_analysis(pre, post, cpus, args, own_cpu=None, wall=None, quiet=None, pin_failures=0):
    window, per_cpu, missing, system = window_stats(pre, post, cpus)
    reasons, notes = [], []
    if window <= 0:
        raise SnapshotError(f"post snapshot is not later than pre ({window:.3f}s)")
    busy = sum(per_cpu.values())
    if missing:
        reasons.append(f"no /proc/stat data for cpu {','.join(map(str, missing))}")
    if own_cpu is None:
        reasons.append("own CPU time unknown: foreign load cannot be separated")
        own_cores = None
        foreign_raw = None
        foreign = None
        other = None
    else:
        own_cores = own_cpu / window
        foreign_raw = busy - own_cores
        foreign = max(0.0, foreign_raw)
        other = max(0.0, system - own_cores)
        if foreign > args.max_foreign:
            reasons.append(f"foreign load {foreign:.3f} cores on CPUs {fmt_cpus(cpus)} > {args.max_foreign:g}")
        if other > args.max_other:
            reasons.append(f"load besides the bench {other:.2f} cores on all CPUs > {args.max_other:g}")
        # The accounting premise (the bench ran on the watched CPUs, inside the
        # window) does not hold: the round cannot be judged.
        if foreign_raw < -0.25:
            reasons.append(f"own CPU exceeds the watched CPUs' busy time by {-foreign_raw:.2f} cores: "
                           "bench not pinned to the watched set")
    if wall is not None and wall > window + 0.5:
        reasons.append(f"bench wall {wall:.2f}s longer than the snapshot window {window:.2f}s")
    if pin_failures:
        reasons.append(f"{pin_failures} bench thread(s) could not be pinned (PIN_FAILURES)")
    fp_start, fp_end = foreign_procs(pre, args.foreign), foreign_procs(post, args.foreign)
    if fp_start:
        reasons.append(f"{len(fp_start)} foreign benchmark process(es) at start: "
                       + "; ".join(p["args"][:60] for p in fp_start[:3]))
    if fp_end:
        reasons.append(f"{len(fp_end)} foreign benchmark process(es) at end: "
                       + "; ".join(p["args"][:60] for p in fp_end[:3]))
    if quiet is not None and not quiet["quiet"]:
        reasons.extend(quiet["reasons"])
    freqs = [s["freq"][c] for s in (pre, post) for c in cpus if c in s["freq"]]
    if freqs and min(freqs) != max(freqs):
        # Sampled outside the load only: a hint, not a verdict.
        notes.append(f"cpufreq of the watched CPUs not constant around the round: {min(freqs)}..{max(freqs)} kHz")
    result = {
        "verdict": "CLEAN" if not reasons else "DISTURBED",
        "reasons": reasons,
        "notes": notes,
        "cpus": fmt_cpus(cpus),
        "window_s": round(window, 3),
        "wall_s": wall,
        "own_cpu_s": own_cpu,
        "own_cores": None if own_cores is None else round(own_cores, 4),
        "watched_busy_cores": round(busy, 4),
        "foreign_cores": None if foreign is None else round(foreign, 4),
        "foreign_cores_raw": None if foreign_raw is None else round(foreign_raw, 4),
        "system_busy_cores": round(system, 3),
        "other_busy_cores": None if other is None else round(other, 3),
        "pin_failures": pin_failures,
        "per_cpu_busy": {str(c): round(v, 4) for c, v in per_cpu.items()},
        "foreign_bench_start": len(fp_start),
        "foreign_bench_end": len(fp_end),
        "freq_min_khz": min(freqs) if freqs else None,
        "freq_max_khz": max(freqs) if freqs else None,
        "freq_changed": bool(freqs) and min(freqs) != max(freqs),
        "quiet": quiet,
        "top": top_procs(pre, post, window),
        "thresholds": {"max_foreign_cores": args.max_foreign, "max_other_cores": args.max_other,
                       "quiet_cpu_busy": args.quiet_cpu_busy,
                       "quiet_min_window_s": args.quiet_min_window, "foreign_pattern": args.foreign},
    }
    return result


def fmt_cpus(cpus):
    out, start, prev = [], None, None
    for c in cpus:
        if start is None:
            start = prev = c
        elif c == prev + 1:
            prev = c
        else:
            out.append(f"{start}-{prev}" if prev > start else str(start))
            start = prev = c
    if start is not None:
        out.append(f"{start}-{prev}" if prev > start else str(start))
    return ",".join(out)


def verdict_line(r):
    line = (f"VERDICT {r['verdict']} window={r['window_s']:.2f}s busy={r['watched_busy_cores']:.3f} "
            f"own={fmt(r['own_cores'])} foreign={fmt(r['foreign_cores'])} (raw {fmt(r['foreign_cores_raw'])}) "
            f"system={r['system_busy_cores']:.2f} other={fmt(r['other_busy_cores'])} foreign_bench={r['foreign_bench_start']}/{r['foreign_bench_end']} "
            f"freq={r['freq_min_khz']}..{r['freq_max_khz']}")
    if r["reasons"]:
        line += " reasons=" + " | ".join(r["reasons"])
    return line


def fmt(v):
    return "n/a" if v is None else f"{v:.3f}"


def percentile(vals, q):
    s = sorted(vals)
    if not s:
        return None
    k = (len(s) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def windows_of(snaps, length):
    """Consecutive, non-overlapping (first, last) snapshot pairs spanning >= length seconds."""
    out, i = [], 0
    for j in range(1, len(snaps)):
        if snaps[j]["uptime"] - snaps[i]["uptime"] >= length:
            out.append((snaps[i], snaps[j]))
            i = j
    return out


def idle_runs(snaps, idle):
    """The maximal runs of consecutive snapshots without a foreign benchmark."""
    runs, cur = [], []
    for s, ok in zip(snaps, idle):
        if ok:
            cur.append(s)
        else:
            runs.append(cur)
            cur = []
    runs.append(cur)
    return runs


def ceil_to(v, step):
    return round(math.ceil(v / step - 1e-9) * step, 4)


def baseline(snaps, cpus, foreign, round_window=None):
    # A window with a foreign benchmark in one of its snapshots is not idle: it is
    # counted, left out of the statistics.
    idle = [not foreign_procs(s, foreign) for s in snaps]
    pairs = list(zip(snaps, snaps[1:]))
    if not pairs:
        raise SnapshotError("baseline needs at least two snapshots")
    per_cpu, watched, system, windows = {c: [] for c in cpus}, [], [], []
    fprocs = 0
    for (a, b), ok_a, ok_b in zip(pairs, idle, idle[1:]):
        if not (ok_a and ok_b):
            fprocs += 1
            continue
        window, pc, _, sysb = window_stats(a, b, cpus)
        windows.append(window)
        for c, v in pc.items():
            per_cpu[c].append(v)
        watched.append(sum(pc.values()))
        system.append(sysb)
    allv = [v for vs in per_cpu.values() for v in vs]

    def stats(vs):
        return {"median": round(statistics.median(vs), 4), "p95": round(percentile(vs, 0.95), 4),
                "max": round(max(vs), 4)} if vs else None

    res = {"pairs": len(windows), "window_s": stats(windows), "cpus": fmt_cpus(cpus),
           "watched_busy_cores": stats(watched), "system_busy_cores": stats(system),
           "per_cpu_busy_all": stats(allv), "per_cpu_busy_max": {str(c): round(max(v), 4) for c, v in per_cpu.items() if v},
           "pairs_with_foreign_bench": fprocs}
    if round_window:
        wins = [w for run in idle_runs(snaps, idle) for w in windows_of(run, round_window)]
        if not wins:
            raise SnapshotError(f"no idle window of {round_window:g}s in the snapshots")
        ww, wsys, wlen = [], [], []
        for a, b in wins:
            length, pc, _, sysb = window_stats(a, b, cpus)
            wlen.append(length)
            ww.append(sum(pc.values()))
            wsys.append(sysb)
        res["round_window"] = {
            "requested_s": round_window, "windows": len(wins), "window_s": stats(wlen),
            "watched_busy_cores": stats(ww), "system_busy_cores": stats(wsys),
            "watched_over_default_max_foreign": round(sum(v > DEFAULT_MAX_FOREIGN for v in ww) / len(ww), 4),
            "system_over_default_max_other": round(sum(v > DEFAULT_MAX_OTHER for v in wsys) / len(wsys), 4),
            # Idle maxima over windows of a round's length, rounded up: an idle
            # machine passes; a round above them had more than idle noise.
            "suggested": {"max_foreign": max(DEFAULT_MAX_FOREIGN, ceil_to(max(ww), 0.05)),
                          "max_other": ceil_to(max(wsys), 0.05)},
        }
    return res


def add_thresholds(p):
    p.add_argument("--cpus", required=True, help="watched CPUs, e.g. 12-19 or 12,13,14")
    p.add_argument("--foreign", default=DEFAULT_FOREIGN, help="regex of foreign benchmark command lines")
    p.add_argument("--max-foreign", type=float, default=DEFAULT_MAX_FOREIGN,
                   help="max foreign cores on the watched CPUs during a round (default %(default)s)")
    p.add_argument("--max-other", type=float, default=DEFAULT_MAX_OTHER,
                   help="max cores busy on all CPUs besides the bench during a round (default %(default)s)")
    p.add_argument("--quiet-cpu-busy", type=float, default=DEFAULT_QUIET_CPU_BUSY,
                   help="max busy fraction of each watched CPU in the quiet window (default %(default)s)")
    p.add_argument("--quiet-min-window", type=float, default=DEFAULT_QUIET_MIN_WINDOW,
                   help="min quiet window in seconds (default %(default)s)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("snippet", help="print the snapshot snippet")
    q = sub.add_parser("quiet", help="pre-round quiet check of the window A -> B")
    add_thresholds(q)
    q.add_argument("a")
    q.add_argument("b")
    q.add_argument("--json")
    r = sub.add_parser("round", help="verdict of one round")
    add_thresholds(r)
    r.add_argument("--pre", required=True)
    r.add_argument("--post", required=True)
    r.add_argument("--quiet-pre", help="snapshot that starts the quiet window ending at --pre")
    r.add_argument("--time", help="bench log with the `time -p` output (last real/user/sys) and PIN_FAILURES lines")
    r.add_argument("--own-cpu", type=float, help="CPU seconds of the bench (instead of --time)")
    r.add_argument("--json", help="write the analysis here")
    b = sub.add_parser("baseline", help="idle statistics over consecutive snapshots")
    b.add_argument("--cpus", required=True)
    b.add_argument("--foreign", default=DEFAULT_FOREIGN)
    b.add_argument("--window", type=float, help="also statistics over windows of about this many seconds")
    b.add_argument("--json")
    b.add_argument("snapshots", nargs="+")
    args = ap.parse_args(argv)

    if args.cmd == "snippet":
        sys.stdout.write(SNIPPET)
        return 0
    try:
        cpus = parse_cpus(args.cpus)
        if args.cmd == "quiet":
            res = quiet_check(load_snapshot(args.a), load_snapshot(args.b), cpus, args)
            print(("QUIET yes" if res["quiet"] else "QUIET no") +
                  f" window={res['window_s']:.2f}s watched_busy={res['watched_busy_cores']:.3f}" +
                  ("" if res["quiet"] else " reasons=" + " | ".join(res["reasons"])))
            if args.json:
                Path(args.json).write_text(json.dumps(res, indent=1) + "\n")
            return 0 if res["quiet"] else 1
        if args.cmd == "round":
            pre, post = load_snapshot(args.pre), load_snapshot(args.post)
            quiet = quiet_check(load_snapshot(args.quiet_pre), pre, cpus, args) if args.quiet_pre else None
            own, wall, pins = args.own_cpu, None, 0
            if args.time:
                text = Path(args.time).read_text(errors="replace")
                pins = parse_pin_failures(text)
                t = parse_time_output(text)
                if t is not None:
                    own, wall = t["user"] + t["sys"], t["real"]
            res = round_analysis(pre, post, cpus, args, own, wall, quiet, pins)
            print(verdict_line(res))
            for n in res["notes"]:
                print(f"NOTE {n}")
            if args.json:
                Path(args.json).write_text(json.dumps(res, indent=1) + "\n")
            return 0 if res["verdict"] == "CLEAN" else 1
        res = baseline([load_snapshot(s) for s in args.snapshots], cpus, args.foreign, args.window)
        print(json.dumps(res, indent=1))
        if args.json:
            Path(args.json).write_text(json.dumps(res, indent=1) + "\n")
        return 0
    except (SnapshotError, ValueError, OSError) as e:
        print(f"perf_env: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
