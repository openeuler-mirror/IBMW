#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

import sys
import csv
import argparse

def main():
    parser = argparse.ArgumentParser(description="A/B p99 performance comparator")
    parser.add_argument("baseline", help="Baseline CSV file")
    parser.add_argument("current", help="Current CSV file to verify")
    parser.add_argument("--threshold", type=float, default=0.05, 
                        help="Maximum allowed deviation (default 0.05 = 5%)")
    args = parser.parse_args()

    def read_p99(filename):
        data = {}
        with open(filename, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                key = (row['Sample'], row['Payload'])
                data[key] = float(row['p99'])
        return data

    try:
        baseline_data = read_p99(args.baseline)
        current_data = read_p99(args.current)
    except Exception as e:
        print(f"ERROR: Failed to read CSV: {e}")
        sys.exit(64)

    failures = []
    print(f"{'Sample':<30} {'Bytes':<10} {'Base p99':<10} {'Curr p99':<10} {'Delta':<10} {'Status'}")
    print("-" * 80)

    for key, base_val in baseline_data.items():
        if key not in current_data:
            print(f"WARNING: Key {key} missing in current data")
            continue
        
        curr_val = current_data[key]
        delta = (curr_val - base_val) / base_val if base_val > 0 else 0
        
        status = "PASS"
        if delta > args.threshold:
            status = "FAIL"
            failures.append((key, delta))
        
        print(f"{key[0]:<30} {key[1]:<10} {base_val:<10.2f} {curr_val:<10.2f} {delta*100:>+6.1f}%    {status}")

    if failures:
        print(f"\nFAIL: {len(failures)} payload(s) exceeded the {args.threshold*100}% threshold.")
        sys.exit(1)
    
    print(f"\nPASS: All payloads within {args.threshold*100}% threshold.")
    sys.exit(0)

if __name__ == "__main__":
    main()
