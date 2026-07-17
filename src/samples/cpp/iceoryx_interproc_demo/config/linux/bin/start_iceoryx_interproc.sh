#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper for the two-process iceoryx demo:
#   1. launches iox-roudi (background)
#   2. launches iceoryx_interproc_subscriber_app (background; started first
#      so its Listener is wired before the first publish)
#   3. launches iceoryx_interproc_publisher_app (foreground)
#   4. shuts everything down on exit.
#
# Usage:
#   ./start_iceoryx_interproc.sh           # run forever (Ctrl-C to stop)
#   DURATION=15 ./start_iceoryx_interproc.sh   # auto-stop after 15s

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

ROUDI_BIN="${SCRIPT_DIR}/iox-roudi"
PUB_BIN="${SCRIPT_DIR}/iceoryx_interproc_publisher_app"
SUB_BIN="${SCRIPT_DIR}/iceoryx_interproc_subscriber_app"
PUB_CFG="${SCRIPT_DIR}/cfg/publisher.yaml"
SUB_CFG="${SCRIPT_DIR}/cfg/subscriber.yaml"
DURATION="${DURATION:-0}"

for f in "${ROUDI_BIN}" "${PUB_BIN}" "${SUB_BIN}"; do
  if [[ ! -x "${f}" ]]; then
    echo "missing executable: ${f}" >&2
    exit 1
  fi
done

echo "[start_iceoryx_interproc.sh] launching iox-roudi (background)..."
"${ROUDI_BIN}" >roudi.log 2>&1 &
ROUDI_PID=$!

cleanup() {
  echo "[start_iceoryx_interproc.sh] cleanup..."
  [[ -n "${PUB_PID:-}" ]] && kill -INT  "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -INT  "${SUB_PID}" 2>/dev/null || true
  sleep 0.3
  [[ -n "${PUB_PID:-}" ]] && kill -KILL "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -KILL "${SUB_PID}" 2>/dev/null || true
  kill "${ROUDI_PID}" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

# Give RouDi a moment to set up shared memory and IPC channels.
sleep 1

echo "[start_iceoryx_interproc.sh] launching subscriber (background)..."
"${SUB_BIN}" "${SUB_CFG}" >subscriber.log 2>&1 &
SUB_PID=$!

# Subscriber needs a brief moment to call PoshRuntime::initRuntime and wire
# its Listener callback before the publisher starts emitting samples.
sleep 1

echo "[start_iceoryx_interproc.sh] launching publisher (background)..."
"${PUB_BIN}" "${PUB_CFG}" >publisher.log 2>&1 &
PUB_PID=$!

if [[ "${DURATION}" != "0" ]]; then
  echo "[start_iceoryx_interproc.sh] running for ${DURATION}s ..."
  sleep "${DURATION}"
else
  echo "[start_iceoryx_interproc.sh] running until Ctrl-C ..."
  wait "${PUB_PID}" || true
fi
