#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper for the two-process shm demo (self-developed shm
# transport, no RouDi daemon):
#   1. launches shm_interproc_subscriber_app (background; started first so
#      its subscription exists before the first publish)
#   2. launches shm_interproc_publisher_app (background)
#   3. shuts both down on exit.
#
# Usage:
#   ./start_shm_interproc.sh                 # run until Ctrl-C
#   DURATION=15 ./start_shm_interproc.sh     # auto-stop after 15s
#   IBMW_SHM_ROOT=/some/dir ./start_shm_interproc.sh   # private segment root
#
# Logs: shm_interproc_subscriber.log / shm_interproc_publisher.log next to
# this script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PUB_BIN="${SCRIPT_DIR}/shm_interproc_publisher_app"
SUB_BIN="${SCRIPT_DIR}/shm_interproc_subscriber_app"
PUB_CFG="${SCRIPT_DIR}/cfg/shm_interproc_publisher.yaml"
SUB_CFG="${SCRIPT_DIR}/cfg/shm_interproc_subscriber.yaml"
DURATION="${DURATION:-0}"

for f in "${PUB_BIN}" "${SUB_BIN}"; do
  if [[ ! -x "${f}" ]]; then
    echo "missing executable: ${f}" >&2
    exit 1
  fi
done
if [[ ! -e "${SCRIPT_DIR}/libibmw_shm_transport.so" ]]; then
  echo "missing build artifact: ${SCRIPT_DIR}/libibmw_shm_transport.so" >&2
  exit 1
fi

cleanup() {
  echo "[start_shm_interproc.sh] cleanup..."
  [[ -n "${PUB_PID:-}" ]] && kill -INT "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -INT "${SUB_PID}" 2>/dev/null || true
  [[ -n "${PUB_PID:-}" ]] && wait "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && wait "${SUB_PID}" 2>/dev/null || true
}
trap cleanup EXIT

echo "[start_shm_interproc.sh] launching subscriber (background)..."
"${SUB_BIN}" "${SUB_CFG}" >shm_interproc_subscriber.log 2>&1 &
SUB_PID=$!

# Let the subscriber register its subscription before the first publish.
sleep 1

echo "[start_shm_interproc.sh] launching publisher (background)..."
"${PUB_BIN}" "${PUB_CFG}" >shm_interproc_publisher.log 2>&1 &
PUB_PID=$!

if [[ "${DURATION}" != "0" ]]; then
  echo "[start_shm_interproc.sh] running for ${DURATION}s ..."
  sleep "${DURATION}"
else
  echo "[start_shm_interproc.sh] running until Ctrl-C ..."
  wait "${PUB_PID}" || true
fi
