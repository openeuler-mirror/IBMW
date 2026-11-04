#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper: runs the single-process shm_inproc_app over the
# self-developed shm transport. No RouDi daemon is needed.
#
# Usage:
#   ./start_shm_inproc.sh                 # run until Ctrl-C
#   DURATION=10 ./start_shm_inproc.sh     # auto-stop after 10s
#   IBMW_SHM_ROOT=/some/dir ./start_shm_inproc.sh   # private segment root

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

APP_BIN="${SCRIPT_DIR}/shm_inproc_app"
CFG="${SCRIPT_DIR}/cfg/shm_inproc_cfg.yaml"
DURATION="${DURATION:-0}"

for f in "${APP_BIN}" "${SCRIPT_DIR}/libibmw_shm_transport.so"; do
  if [[ ! -e "${f}" ]]; then
    echo "missing build artifact: ${f}" >&2
    exit 1
  fi
done

echo "[start_shm_inproc.sh] running ${APP_BIN} ${CFG}"
if [[ "${DURATION}" == "0" ]]; then
  exec "${APP_BIN}" "${CFG}"
fi

"${APP_BIN}" "${CFG}" &
APP_PID=$!
trap 'kill -INT "${APP_PID}" 2>/dev/null || true; wait "${APP_PID}" 2>/dev/null || true' EXIT
sleep "${DURATION}"
