#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper for the iox_loan_v2 two-process benchmark:
#   1. launches iox-roudi (background)
#   2. launches iox_loan_v2_subscriber_app (background; started first
#      so its Listener is wired before the first publish)
#   3. launches iox_loan_v2_publisher_app (background; foreground-waited)
#   4. shuts everything down on exit.
#
# The subscriber appends a summary row to the csv_path declared in
# cfg/iox_loan_v2/subscriber.yaml (default /tmp/ibmw_bench/iox_bench_lat.csv) at Stop().
#
# Usage:
#   ./start_iox_loan_v2.sh               # auto-stop after duration_sec+5s
#   DURATION=15 ./start_iox_loan_v2.sh   # override stop deadline (seconds)
#   FOREVER=1 ./start_iox_loan_v2.sh     # run until Ctrl-C

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Keep all Iceoryx clients on the same build/runtime dependency set.  The
# extension is loaded from SCRIPT_DIR by the YAML config, while the binaries'
# RUNPATH may point at the parent build directory; loading both copies would
# create two independent PoshRuntime singletons in one process.
export LD_LIBRARY_PATH="${SCRIPT_DIR}:${LD_LIBRARY_PATH:-}:/opt/ros/${ROS_DISTRO:-humble}/lib"

ROUDI_BIN="${SCRIPT_DIR}/iox-roudi"
PUB_BIN="${SCRIPT_DIR}/iox_loan_v2_publisher_app"
SUB_BIN="${SCRIPT_DIR}/iox_loan_v2_subscriber_app"
PUB_CFG="${SCRIPT_DIR}/cfg/iox_loan_v2/publisher.yaml"
SUB_CFG="${SCRIPT_DIR}/cfg/iox_loan_v2/subscriber.yaml"

# Default DURATION = duration_sec from publisher.yaml + 5s grace.
# duration_sec lives under the per-module subtree (e.g.
# IoxLoanV2PublisherModule.bench_options.duration_sec); a relaxed grep
# tolerates any indent.
if [[ -z "${DURATION:-}" ]]; then
  DUR_FROM_YAML=$(grep -E '^\s*duration_sec:' "${PUB_CFG}" | head -1 \
                  | awk '{print $2}' || true)
  if [[ -n "${DUR_FROM_YAML}" ]]; then
    DURATION=$((DUR_FROM_YAML + 5))
  else
    DURATION=35
  fi
fi
[[ "${FOREVER:-0}" == "1" ]] && DURATION=0

# csv_path side-effect: ensure parent dir exists for the subscriber's append.
CSV_PATH=$(grep -E '^\s*csv_path:' "${SUB_CFG}" | head -1 \
           | awk '{print $2}' || true)
if [[ -n "${CSV_PATH}" ]]; then
  mkdir -p "$(dirname "${CSV_PATH}")"
fi

for f in "${ROUDI_BIN}" "${PUB_BIN}" "${SUB_BIN}"; do
  if [[ ! -x "${f}" ]]; then
    echo "missing executable: ${f}" >&2
    exit 1
  fi
done

echo "[start_iox_loan_v2.sh] launching iox-roudi (background)..."
"${ROUDI_BIN}" >roudi.log 2>&1 &
ROUDI_PID=$!

cleanup() {
  echo "[start_iox_loan_v2.sh] cleanup..."
  [[ -n "${PUB_PID:-}" ]] && kill -INT  "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -INT  "${SUB_PID}" 2>/dev/null || true
  sleep 0.5
  [[ -n "${PUB_PID:-}" ]] && kill -KILL "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -KILL "${SUB_PID}" 2>/dev/null || true
  kill "${ROUDI_PID}" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

# Give RouDi a moment to set up shared memory and IPC channels.
sleep 1

echo "[start_iox_loan_v2.sh] launching subscriber (background)..."
"${SUB_BIN}" "${SUB_CFG}" >subscriber.log 2>&1 &
SUB_PID=$!

# Subscriber needs to call PoshRuntime::initRuntime and wire its Listener
# callback before the publisher emits the first sample.
sleep 1

echo "[start_iox_loan_v2.sh] launching publisher (background)..."
"${PUB_BIN}" "${PUB_CFG}" >publisher.log 2>&1 &
PUB_PID=$!

if [[ "${DURATION}" != "0" ]]; then
  echo "[start_iox_loan_v2.sh] running for ${DURATION}s ..."
  sleep "${DURATION}"
else
  echo "[start_iox_loan_v2.sh] running until Ctrl-C ..."
  wait "${PUB_PID}" || true
fi
