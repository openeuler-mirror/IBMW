#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Summary of interleaved ibmw_shm_ext_bench runs: <dir>/<label>.<round>.log.

Prints the per-round ns/op and, per case, the median and min..max of each label.
With labels base and new it adds the delta of the medians and the median and range
of the per-round paired delta (round i of new minus round i of base). The paired
median is the one to read when a machine changes speed during a run: the delta of
two medians can then compare a fast round with a slow one. Rows also show Mops/s.

Usage: shm_ext_bench_summary.py <dir> [<base label> <new label>]. With two labels
only those are compared (a directory may hold more, e.g. three interleaved arms).
"""
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"^RESULT case=(\S+) payload=(\d+) threads=(\d+) ns_per_op=(\S+) mops=(\S+) "
                  r"rep_min_mops=\S+ rep_max_mops=\S+ ops=(\d+) fails=(\d+) delivered=(\d+)")


def main(root, pair=None):
    data = defaultdict(lambda: defaultdict(dict))  # key -> label -> round -> (ns, mops, ops, fails, delivered)
    labels, pin_failures = set(), 0
    for path in sorted(Path(root).glob("*.*.log")):
        label, round_no = path.name.split(".")[:2]
        labels.add(label)
        for line in path.read_text(errors="replace").splitlines():
            m = LINE.match(line)
            if m:
                key = (m[1], int(m[2]), int(m[3]))
                data[key][label][int(round_no)] = (float(m[4]), float(m[5]), int(m[6]), int(m[7]), int(m[8]))
            elif line.startswith("PIN_FAILURES="):
                pin_failures += int(line.split("=")[1])
    if pair:
        missing = [x for x in pair if x not in labels]
        if missing:
            print(f"labels {missing} not found in {root}", file=sys.stderr)
            return 1
        order = list(pair)
    else:
        order = [x for x in ("base", "new") if x in labels] or sorted(labels)

    print("## per-round ns/op (Mops/s for scale_*)")
    for key in data:
        for label in order:
            rows = data[key][label]
            vals = [rows[r][1 if key[0].startswith("scale") else 0] for r in sorted(rows)]
            print(f"{key[0]} {key[1]}B x{key[2]} {label}: " + " ".join(f"{v:.2f}" for v in vals))
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
            row += (f" {d:+.1f} | {100 * d / med[a]:+.1f}% | {statistics.median(paired):+.1f} [{min(paired):+.1f}..{max(paired):+.1f}] | "
                    f"{mops[a]:.2f} -> {mops[b]:.2f} |")
        else:
            row += f" {mops[order[0]]:.2f} |"
        print(row)
    subs = [k for k in data if k[0].endswith("_sub")]
    for key in subs:
        for label in order:
            vals = data[key][label].values()
            print(f"{key[0]} {key[1]}B {label}: delivered/ops median "
                  f"{statistics.median(v[4] / max(v[2], 1) for v in vals):.3f}, "
                  f"fails/ops median {statistics.median(v[3] / max(v[2], 1) for v in vals):.3f}")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 4):
        print(__doc__, file=sys.stderr)
        sys.exit(64)
    sys.exit(main(sys.argv[1], sys.argv[2:4] if len(sys.argv) == 4 else None))
