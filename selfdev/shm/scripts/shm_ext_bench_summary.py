#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Summary of interleaved ibmw_shm_ext_bench runs: <dir>/<label>.<round>.log.

Prints the per-round ns/op and, per case, the median and min..max of each label.
With labels base and new it adds the delta of the medians and the median and range
of the per-round paired delta (round i of new minus round i of base). The paired
median is the one to read when a machine changes speed during a run: the delta of
two medians can then compare a fast round with a slow one. Rows also show Mops/s.

Usage: shm_ext_bench_summary.py [--clean-only [--min-clean N] [--planned N]] <dir> [<base label> <new label>].
With two labels only those are compared (a directory may hold more, e.g. three
interleaved arms).

Interference (run_shm_ohos.sh, perf_env.py): a round may have a verdict file
<label>.<round>.env.json ({"verdict": "CLEAN"|"DISTURBED", "reasons": [...]}).
Without --clean-only every round is used, as before, and the verdicts found are
only counted. With --clean-only only CLEAN rounds are used (a round without a
verdict file counts as not clean), paired deltas only use rounds that are clean
in both labels, and the disturbed rounds are listed with their reasons. When a
label, or (with two labels) the rounds clean in both, number fewer than
--min-clean (default: two thirds of --planned, the rounds the run was meant to
have, else of the rounds found, rounded up) the summary says INTERFERENCE
INCONCLUSIVE and exits 3. --clean-only also lists per-case outliers among the
clean rounds (a value more than max(5 MAD, 10%) from the median of its label):
a short burst of foreign load can hit one case of a round that is clean as a
whole; they are listed, not removed.
"""
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"^RESULT case=(\S+) payload=(\d+) threads=(\d+) ns_per_op=(\S+) mops=(\S+) "
                  r"rep_min_mops=\S+ rep_max_mops=\S+ ops=(\d+) fails=(\d+) delivered=(\d+)")


def load_verdicts(root):
    """(label, round) -> (verdict, reasons) from <label>.<round>.env.json."""
    verdicts = {}
    for path in Path(root).glob("*.*.env.json"):
        label, round_no = path.name.split(".")[:2]
        if not round_no.isdigit():
            continue
        try:
            env = json.loads(path.read_text())
            verdicts[(label, int(round_no))] = (env.get("verdict", "UNKNOWN"), env.get("reasons", []))
        except (OSError, ValueError):
            verdicts[(label, int(round_no))] = ("UNKNOWN", [f"unreadable {path.name}"])
    return verdicts


def main(root, pair=None, clean_only=False, min_clean=None, planned=None):
    data = defaultdict(lambda: defaultdict(dict))  # key -> label -> round -> (ns, mops, ops, fails, delivered)
    labels, pin_failures, pin_by_label = set(), 0, defaultdict(int)
    verdicts = load_verdicts(root)
    rounds_of = defaultdict(set)
    for path in sorted(Path(root).glob("*.*.log")):
        label, round_no = path.name.split(".")[:2]
        if not round_no.isdigit():
            continue
        labels.add(label)
        rounds_of[label].add(int(round_no))
        if clean_only and verdicts.get((label, int(round_no)), ("NONE",))[0] != "CLEAN":
            continue
        for line in path.read_text(errors="replace").splitlines():
            m = LINE.match(line)
            if m:
                key = (m[1], int(m[2]), int(m[3]))
                data[key][label][int(round_no)] = (float(m[4]), float(m[5]), int(m[6]), int(m[7]), int(m[8]))
            elif line.startswith("PIN_FAILURES="):
                pin_failures += int(line.split("=")[1])
                pin_by_label[label] += int(line.split("=")[1])
    if pair:
        missing = [x for x in pair if x not in labels]
        if missing:
            print(f"labels {missing} not found in {root}", file=sys.stderr)
            return 1
        order = list(pair)
    else:
        order = [x for x in ("base", "new") if x in labels] or sorted(labels)

    inconclusive = False
    if verdicts or clean_only:
        print("## interference (perf_env.py verdicts per round)")
        clean_of = {}
        for label in order:
            rounds = sorted(rounds_of[label])
            clean = [r for r in rounds if verdicts.get((label, r), ("NONE",))[0] == "CLEAN"]
            clean_of[label] = set(clean)
            need = min_clean if min_clean is not None else math.ceil(2 * (planned or len(rounds)) / 3)
            print(f"{label}: {len(clean)} clean of {len(rounds)} rounds"
                  + (f", {planned} planned" if planned else "") + f" (minimum {need})")
            for r in rounds:
                v, reasons = verdicts.get((label, r), ("NONE", ["no verdict file"]))
                if v != "CLEAN":
                    print(f"  {label}.{r} {v}: " + " | ".join(reasons))
            if clean_only and len(clean) < need:
                inconclusive = True
        if clean_only and len(order) == 2:
            both = clean_of[order[0]] & clean_of[order[1]]
            need = min_clean if min_clean is not None else math.ceil(
                2 * (planned or max(len(rounds_of[lb]) for lb in order)) / 3)
            print(f"paired: {len(both)} rounds clean in both {order[0]} and {order[1]} (minimum {need})")
            if len(both) < need:
                inconclusive = True
        if clean_only:
            print("INTERFERENCE " + ("INCONCLUSIVE: too few clean rounds; the numbers below are not a result"
                                     if inconclusive else "OK: summary over CLEAN rounds only"))
        else:
            print("(all rounds used; --clean-only restricts the summary to CLEAN rounds)")
        print()
    if not any(data[k][lb] for k in data for lb in order):
        print("no RESULT lines in the selected rounds")
        return 3 if inconclusive else 1

    print("## per-round ns/op (Mops/s for scale_*)")
    for key in data:
        for label in order:
            rows = data[key][label]
            vals = [rows[r][1 if key[0].startswith("scale") else 0] for r in sorted(rows)]
            print(f"{key[0]} {key[1]}B x{key[2]} {label}: " + " ".join(f"{v:.2f}" for v in vals))
    if clean_only:
        print(f"\npin failures in the CLEAN rounds of {', '.join(order)}: {sum(pin_by_label[lb] for lb in order)}")
    else:
        print(f"\npin failures in all runs (every label in the directory): {pin_failures}")
    bad = [(k, lb, r) for k in data for lb in order for r, v in data[k][lb].items()
           if v[3] > v[2] // 100 and not k[0].endswith("_sub")]
    print(f"rounds with more than 1% failed ops: {bad if bad else 'none'}")

    print("\n## summary (ns/op, median [min..max] over rounds)")
    print("| case | payload | threads | " + " | ".join(order) +
          (f" | delta ns | delta % | paired delta median [min..max] | Mops/s {order[0]} -> {order[-1]} |"
           if len(order) == 2 else " | Mops/s |"))
    print("|---|---|---|" + "---|" * (len(order) + (4 if len(order) == 2 else 1)))
    for key in data:
        if not all(data[key][label] for label in order):
            print(f"| {key[0]} | {key[1]} | {key[2]} | no selected rounds for {[lb for lb in order if not data[key][lb]]} |")
            continue
        cells, med = [], {}
        for label in order:
            vals = [v[0] for v in data[key][label].values()]
            med[label] = statistics.median(vals)
            cells.append(f"{med[label]:.1f} [{min(vals):.1f}..{max(vals):.1f}]")
        row = f"| {key[0]} | {key[1]} | {key[2]} | " + " | ".join(cells) + " |"
        mops = {lb: statistics.median(v[1] for v in data[key][lb].values()) for lb in order}
        if len(order) == 2:
            a, b = order
            rounds = sorted(set(data[key][a]) & set(data[key][b]))
            paired = [data[key][b][r][0] - data[key][a][r][0] for r in rounds]
            d = med[b] - med[a]
            pcell = (f"{statistics.median(paired):+.1f} [{min(paired):+.1f}..{max(paired):+.1f}]" if paired
                     else "no common round")
            row += (f" {d:+.1f} | {100 * d / med[a]:+.1f}% | {pcell} | "
                    f"{mops[a]:.2f} -> {mops[b]:.2f} |")
        else:
            row += f" {mops[order[0]]:.2f} |"
        print(row)
    subs = [k for k in data if k[0].endswith("_sub")]
    for key in subs:
        for label in order:
            vals = data[key][label].values()
            if not vals:
                continue
            print(f"{key[0]} {key[1]}B {label}: delivered/ops median "
                  f"{statistics.median(v[4] / max(v[2], 1) for v in vals):.3f}, "
                  f"fails/ops median {statistics.median(v[3] / max(v[2], 1) for v in vals):.3f}")
    if clean_only:
        print("\n## outliers among the CLEAN rounds (|value - median| > max(5 MAD, 10%), ns/op)")
        found = 0
        for key in data:
            for label in order:
                rows = data[key][label]
                if len(rows) < 5:
                    continue
                vals = [v[0] for v in rows.values()]
                med = statistics.median(vals)
                mad = statistics.median(abs(v - med) for v in vals)
                lim = max(5 * mad, 0.10 * med)
                out = [f"{r}:{rows[r][0]:.1f}" for r in sorted(rows) if abs(rows[r][0] - med) > lim]
                if out:
                    found += len(out)
                    print(f"{key[0]} {key[1]}B x{key[2]} {label} (median {med:.1f}): round:value " + " ".join(out))
        if not found:
            print("none")
    return 3 if inconclusive else 0


if __name__ == "__main__":
    argv, clean_only, min_clean, planned = sys.argv[1:], False, None, None
    while argv and argv[0].startswith("--"):
        if argv[0] == "--clean-only":
            clean_only, argv = True, argv[1:]
        elif argv[0] == "--min-clean" and len(argv) > 1 and argv[1].isdigit():
            min_clean, argv = int(argv[1]), argv[2:]
        elif argv[0] == "--planned" and len(argv) > 1 and argv[1].isdigit() and int(argv[1]) > 0:
            planned, argv = int(argv[1]), argv[2:]
        else:
            argv = []
    if len(argv) not in (1, 3):
        print(__doc__, file=sys.stderr)
        sys.exit(64)
    sys.exit(main(argv[0], argv[1:3] if len(argv) == 3 else None, clean_only, min_clean, planned))
