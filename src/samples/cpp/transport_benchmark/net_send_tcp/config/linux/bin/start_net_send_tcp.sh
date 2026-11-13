#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Convenience wrapper for the net_send_tcp two-process benchmark:
#   1. launches net_send_tcp_subscriber_app (background; started first
#      so its OnMessage handler is wired before the first publish)
#   2. launches net_send_tcp_publisher_app (background; foreground-waited)
#   3. shuts everything down on exit.
#
# No iox-roudi, no FastDDS: net_extension's tcp transport driver opens
# a plain asio TCP server socket on the subscriber side
# (tcp_options.listen_port in subscriber.yaml) and a TCP client socket
# on the publisher side (server_url_list in publisher.yaml). No
# discovery protocol -- the wire address is configured statically.
#
# The subscriber appends a summary row to the csv_path declared in
# cfg/net_send_tcp/subscriber.yaml (default /tmp/ibmw_bench/iox_bench_lat.csv)
# at Stop().
#
# Usage:
#   ./start_net_send_tcp.sh               # auto-stop after duration_sec+5s
#   DURATION=15 ./start_net_send_tcp.sh   # override stop deadline (seconds)
#   FOREVER=1 ./start_net_send_tcp.sh     # run until Ctrl-C

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PUB_BIN="${SCRIPT_DIR}/net_send_tcp_publisher_app"
SUB_BIN="${SCRIPT_DIR}/net_send_tcp_subscriber_app"
PUB_CFG="${SCRIPT_DIR}/cfg/net_send_tcp/publisher.yaml"
SUB_CFG="${SCRIPT_DIR}/cfg/net_send_tcp/subscriber.yaml"

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

cleanup() {
  echo "[start_net_send_tcp.sh] cleanup..."
  [[ -n "${PUB_PID:-}" ]] && kill -INT  "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -INT  "${SUB_PID}" 2>/dev/null || true
  sleep 0.5
  [[ -n "${PUB_PID:-}" ]] && kill -KILL "${PUB_PID}" 2>/dev/null || true
  [[ -n "${SUB_PID:-}" ]] && kill -KILL "${SUB_PID}" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

# Poll <file> every 0.1 s until it matches the ERE <pattern>. Fail loudly
# (log tail, non-zero exit) if <pid> dies first or READY_TIMEOUT_SEC elapses.
READY_TIMEOUT_SEC="${READY_TIMEOUT_SEC:-15}"
wait_for_log() {
  local file="$1" pattern="$2" pid="$3" what="$4"
  local deadline=$((SECONDS + READY_TIMEOUT_SEC))
  until grep -Eq -- "${pattern}" "${file}" 2>/dev/null; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      echo "[start_net_send_tcp.sh] ${what} exited before it was ready; tail of ${file}:" >&2
      tail -n 20 "${file}" >&2 || true
      exit 1
    fi
    if (( SECONDS >= deadline )); then
      echo "[start_net_send_tcp.sh] ${what} not ready after ${READY_TIMEOUT_SEC}s; tail of ${file}:" >&2
      tail -n 20 "${file}" >&2 || true
      exit 1
    fi
    sleep 0.1
  done
}

echo "[start_net_send_tcp.sh] launching subscriber (background)..."
"${SUB_BIN}" "${SUB_CFG}" >subscriber.log 2>&1 &
SUB_PID=$!

# Subscriber needs the TCP listener bound (and any net_extension
# warm-up complete) before the publisher attempts its TCP client
# connect. The engine logs "Running..." only after Start() has bound the
# listener.
wait_for_log subscriber.log 'Running( \(async\))?\. Waiting for shutdown signal\.' "${SUB_PID}" subscriber

echo "[start_net_send_tcp.sh] launching publisher (background)..."
"${PUB_BIN}" "${PUB_CFG}" >publisher.log 2>&1 &
PUB_PID=$!

if [[ "${DURATION}" != "0" ]]; then
  echo "[start_net_send_tcp.sh] running for ${DURATION}s ..."
  sleep "${DURATION}"
else
  echo "[start_net_send_tcp.sh] running until Ctrl-C ..."
  wait "${PUB_PID}" || true
fi
