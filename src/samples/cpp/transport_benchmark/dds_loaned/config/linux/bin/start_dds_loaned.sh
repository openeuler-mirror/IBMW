#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper for the dds_loaned two-process benchmark:
#   1. launches dds_loaned_subscriber_app (background; started first
#      so its OnMessage handler is wired before the first publish)
#   2. launches dds_loaned_publisher_app (background; foreground-waited)
#   3. shuts everything down on exit.
#
# No iox-roudi: FastDDS DataSharing manages its own SHM segment.
#
# The subscriber appends a summary row to the csv_path declared in
# cfg/dds_loaned/subscriber.yaml (default /tmp/ibmw_bench/iox_bench_lat.csv) at Stop().
#
# Usage:
#   ./start_dds_loaned.sh               # auto-stop after duration_sec+5s
#   DURATION=15 ./start_dds_loaned.sh   # override stop deadline (seconds)
#   SUBSCRIBER_GRACE_SEC=3 ./start_dds_loaned.sh  # override discovery grace
#   CLEANUP_GRACE_SEC=3 ./start_dds_loaned.sh  # allow graceful shutdown logs
#   IBMW_DDS_LOANED_ASYNC_PROFILE=1 ./start_dds_loaned.sh  # enable profile breadcrumbs
#   FOREVER=1 ./start_dds_loaned.sh     # run until Ctrl-C

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PUB_BIN="${SCRIPT_DIR}/dds_loaned_publisher_app"
SUB_BIN="${SCRIPT_DIR}/dds_loaned_subscriber_app"
PUB_CFG="${SCRIPT_DIR}/cfg/dds_loaned/publisher.yaml"
SUB_CFG="${SCRIPT_DIR}/cfg/dds_loaned/subscriber.yaml"

# Default DURATION = duration_sec from publisher.yaml + 5s grace.
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

for f in "${PUB_BIN}" "${SUB_BIN}"; do
  if [[ ! -x "${f}" ]]; then
    echo "missing executable: ${f}" >&2
    exit 1
  fi
done

if [[ -n "${IBMW_DDS_LOANED_ASYNC_PROFILE:-}" && "${IBMW_DDS_LOANED_ASYNC_PROFILE}" != "0" ]]; then
  export IBMW_TRANSPORT_SHUTDOWN_PROFILE="${IBMW_TRANSPORT_SHUTDOWN_PROFILE:-1}"
fi

cleanup() {
  echo "[start_dds_loaned.sh] cleanup..."
  [[ -n "${PUB_PID:-}" ]] && kill -INT  "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -INT  "${SUB_PID}" 2>/dev/null || true
  sleep "${CLEANUP_GRACE_SEC:-0.5}"
  [[ -n "${PUB_PID:-}" ]] && kill -KILL "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -KILL "${SUB_PID}" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

echo "[start_dds_loaned.sh] launching subscriber (background)..."
"${SUB_BIN}" "${SUB_CFG}" >subscriber.log 2>&1 &
SUB_PID=$!

# Subscriber needs DDS DataReader + OnMessage handler wired before the
# publisher emits the first sample. FastDDS discovery is usually sub-second on
# loopback, but shared hosts can occasionally need longer. Use a 5s default to
# avoid zero-count benchmark rows when sealing DDS loaned performance.
sleep "${SUBSCRIBER_GRACE_SEC:-5}"

echo "[start_dds_loaned.sh] launching publisher (background)..."
"${PUB_BIN}" "${PUB_CFG}" >publisher.log 2>&1 &
PUB_PID=$!

if [[ "${DURATION}" != "0" ]]; then
  echo "[start_dds_loaned.sh] running for ${DURATION}s ..."
  sleep "${DURATION}"
else
  echo "[start_dds_loaned.sh] running until Ctrl-C ..."
  wait "${PUB_PID}" || true
fi
