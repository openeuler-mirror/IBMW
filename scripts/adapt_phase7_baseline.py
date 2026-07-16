#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
import csv
import sys

INPUT_HEADER = [
    "label", "count", "min_us", "avg_us", "stddev_us",
    "p50_us", "p90_us", "p99_us", "p999_us", "max_us",
]
OUTPUT_HEADER = [
    "Sample", "Payload", "p99", "count", "min_us", "avg_us", "stddev_us",
    "p50_us", "p90_us", "p999_us", "max_us",
]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        sys.stderr.write(f"usage: {argv[0]} <input.csv>\n")
        return 2
    with open(argv[1], newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader)
        if header != INPUT_HEADER:
            sys.stderr.write(
                f"error: unexpected input header.\n  got:      {header}\n"
                f"  expected: {INPUT_HEADER}\n"
            )
            return 1
        writer = csv.writer(sys.stdout, lineterminator="\n")
        writer.writerow(OUTPUT_HEADER)
        for row in reader:
            if not row:
                continue
            if len(row) != 10:
                sys.stderr.write(f"error: row has {len(row)} cols, expected 10: {row}\n")
                return 1
            label = row[0]
            sep = label.rfind("_")
            if sep < 0:
                sys.stderr.write(f"error: label has no underscore: {label!r}\n")
                return 1
            sample, payload = label[:sep], label[sep + 1:]
            count, min_us, avg_us, stddev_us, p50, p90, p99, p999, mx = row[1:]
            writer.writerow([sample, payload, p99, count, min_us, avg_us,
                             stddev_us, p50, p90, p999, mx])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
