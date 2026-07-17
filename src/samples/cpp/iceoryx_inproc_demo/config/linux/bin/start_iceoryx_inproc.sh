#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper: launches iox-roudi in the background, runs the
# single-process iceoryx_inproc_app, then shuts roudi down on exit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

ROUDI_BIN="${SCRIPT_DIR}/iox-roudi"
APP_BIN="${SCRIPT_DIR}/iceoryx_inproc_app"
CFG="${SCRIPT_DIR}/cfg/iceoryx_inproc_cfg.yaml"

if [[ ! -x "${ROUDI_BIN}" ]]; then
  echo "iox-roudi not found at ${ROUDI_BIN}" >&2
  exit 1
fi

echo "[start_iceoryx_inproc.sh] launching iox-roudi (background)..."
"${ROUDI_BIN}" >roudi.log 2>&1 &
ROUDI_PID=$!

trap 'echo "[start_iceoryx_inproc.sh] stopping iox-roudi (pid ${ROUDI_PID})"; kill "${ROUDI_PID}" 2>/dev/null || true; wait "${ROUDI_PID}" 2>/dev/null || true' EXIT

# Give RouDi a moment to set up shared memory and IPC channels.
sleep 1

echo "[start_iceoryx_inproc.sh] running ${APP_BIN} ${CFG}"
"${APP_BIN}" "${CFG}"
