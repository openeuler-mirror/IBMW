#!/usr/bin/env python3
import signal
import subprocess
import sys
import time


def start(executable, mode):
    process = subprocess.Popen(
        [executable, mode], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if process.stdout is None or process.stdout.readline().strip() != "SIGNAL_REGRESSION_ACTIVE":
        process.kill()
        raise RuntimeError("signal regression process did not become active")
    return process


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_metrics_signal_shutdown_regression.py EXECUTABLE")

    clean = start(sys.argv[1], "--signal-regression")
    begin = time.monotonic()
    clean.send_signal(signal.SIGTERM)
    stdout, stderr = clean.communicate(timeout=2)
    if clean.returncode != 0 or "SIGNAL_REGRESSION_OWNER_MAIN" not in stdout:
        raise RuntimeError(f"SIGTERM shutdown failed: {clean.returncode}: {stderr}")
    if time.monotonic() - begin >= 2:
        raise RuntimeError("SIGTERM shutdown exceeded its bound")

    stuck = start(sys.argv[1], "--signal-regression-stuck")
    stuck.send_signal(signal.SIGTERM)
    try:
        stuck.wait(timeout=0.5)
    except subprocess.TimeoutExpired:
        stuck.kill()
        stuck.wait(timeout=2)
    else:
        raise RuntimeError("stuck shutdown unexpectedly exited without fallback")
    if stuck.returncode != -signal.SIGKILL:
        raise RuntimeError("stuck shutdown fallback did not use SIGKILL")


if __name__ == "__main__":
    main()
