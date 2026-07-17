#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(pwd)}"
TMP_DIR="${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-dds-send-shm}"
mkdir -p "$TMP_DIR"

DOMAIN_ID="${DOMAIN_ID:-43}"
PAYLOAD_BYTES="${PAYLOAD_BYTES:-4096}"
HZ="${HZ:-100}"
DURATION_SEC="${DURATION_SEC:-4}"
MESSAGE_COUNT="${MESSAGE_COUNT:-0}"
START_DELAY_MS="${START_DELAY_MS:-2000}"
WARMUP_MESSAGES="${WARMUP_MESSAGES:-0}"
DDS_EXECUTOR_CASE="${DDS_EXECUTOR_CASE:-thread_pool}"
CPUSET="${CPUSET:-}"
TOPIC="dds_bench_send_shm_topic"
LABEL="dds_send_shm_executor_e2e_${DDS_EXECUTOR_CASE}_${PAYLOAD_BYTES}"

PUB_APP="${PUB_APP:-${BUILD_DIR}/dds_send_shm_publisher_app}"
SUB_APP="${SUB_APP:-${BUILD_DIR}/dds_send_shm_subscriber_app}"
DDS_LIB="${DDS_LIB:-${BUILD_DIR}/libibmw_dds_transport.so}"

PUB_CFG="${TMP_DIR}/ibmw_dds_send_shm_executor_e2e_${DDS_EXECUTOR_CASE}_publisher_${DOMAIN_ID}.yaml"
SUB_CFG="${TMP_DIR}/ibmw_dds_send_shm_executor_e2e_${DDS_EXECUTOR_CASE}_subscriber_${DOMAIN_ID}.yaml"
PUB_LOG="${TMP_DIR}/ibmw_dds_send_shm_executor_e2e_${DDS_EXECUTOR_CASE}_publisher_${DOMAIN_ID}.log"
SUB_LOG="${TMP_DIR}/ibmw_dds_send_shm_executor_e2e_${DDS_EXECUTOR_CASE}_subscriber_${DOMAIN_ID}.log"
CSV_PATH="${CSV_PATH:-${TMP_DIR}/ibmw_dds_send_shm_executor_e2e_${DDS_EXECUTOR_CASE}_${PAYLOAD_BYTES}_${DOMAIN_ID}_$$.csv}"

case "${PAYLOAD_BYTES}" in
  1024) PAYLOAD_LABEL="1KB" ;;
  4096) PAYLOAD_LABEL="4KB" ;;
  16384) PAYLOAD_LABEL="16KB" ;;
  65536) PAYLOAD_LABEL="64KB" ;;
  262144) PAYLOAD_LABEL="256KB" ;;
  1048576) PAYLOAD_LABEL="1MB" ;;
  4194304) PAYLOAD_LABEL="4MB" ;;
  *)
    echo "FAIL: unsupported PAYLOAD_BYTES='${PAYLOAD_BYTES}'" >&2
    exit 2
    ;;
esac
QOS_TOPIC_PATTERN=".*BenchSendPayload${PAYLOAD_LABEL}_NonPlain"

RUN_PREFIX=()
if [[ -n "${CPUSET}" ]]; then
  RUN_PREFIX=(taskset -c "${CPUSET}")
fi

case "${DDS_EXECUTOR_CASE}" in
  thread_pool)
    EXECUTOR_BLOCK="      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2"
    ;;
  strand)
    EXECUTOR_BLOCK="      - name: dds_send_shm_strand_base_pool
        type: thread_pool
        options:
          thread_num: 2
      - name: work_thread_pool
        type: strand
        options:
          bind_thread_pool_executor_name: dds_send_shm_strand_base_pool"
    ;;
  dedicated)
    EXECUTOR_BLOCK="      - name: work_thread_pool
        type: dedicated"
    ;;
  parallel)
    EXECUTOR_BLOCK="      - name: work_thread_pool
        type: parallel
        options:
          thread_num: 2"
    ;;
  *)
    echo "FAIL: unsupported DDS_EXECUTOR_CASE='${DDS_EXECUTOR_CASE}'" >&2
    exit 2
    ;;
esac

cleanup() {
  if [[ -n "${PUB_PID:-}" ]] && kill -0 "$PUB_PID" 2>/dev/null; then
    kill -TERM "$PUB_PID" 2>/dev/null || true
    sleep 1
    kill -KILL "$PUB_PID" 2>/dev/null || true
    wait "$PUB_PID" 2>/dev/null || true
  fi
  if [[ -n "${SUB_PID:-}" ]] && kill -0 "$SUB_PID" 2>/dev/null; then
    kill -TERM "$SUB_PID" 2>/dev/null || true
    sleep 1
    kill -KILL "$SUB_PID" 2>/dev/null || true
    wait "$SUB_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

terminate_process() {
  local pid="$1"

  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      local stat
      stat="$(ps -p "$pid" -o stat= 2>/dev/null || true)"
      if [[ "$stat" == Z* ]]; then
        break
      fi
      sleep 1
    done
  fi

  if kill -0 "$pid" 2>/dev/null; then
    local stat
    stat="$(ps -p "$pid" -o stat= 2>/dev/null || true)"
    if [[ "$stat" != Z* ]]; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
  wait "$pid" 2>/dev/null || true
}

cat >"$PUB_CFG" <<EOF
ibmw:
  extensions:
    items:
      - name: dds_extension
        path: ${DDS_LIB}
        options:
          domain_id: ${DOMAIN_ID}
          transport_policy: shm_only
  logging:
    core_level: INFO
    backends:
      - type: console
  scheduling:
    executors:
${EXECUTOR_BLOCK}
  messaging:
    drivers:
      - type: dds
        options:
          domain_id: ${DOMAIN_ID}
          ros2_compatible: false
          publishers:
            - topic: "${QOS_TOPIC_PATTERN}"
              qos:
                history: keep_last
                depth: 8192
                reliability: reliable
                durability: volatile
    publishers:
      - topic: "${TOPIC}"
        enable_drivers: [dds]
  modules:
    modules:
      - name: DdsSendPublisherModule
        log_level: INFO
DdsSendPublisherModule:
  bench_options:
    payload_bytes: ${PAYLOAD_BYTES}
    hz: ${HZ}
    duration_sec: ${DURATION_SEC}
    message_count: ${MESSAGE_COUNT}
    start_delay_ms: ${START_DELAY_MS}
    label: ${LABEL}
    warmup_messages: ${WARMUP_MESSAGES}
EOF

cat >"$SUB_CFG" <<EOF
ibmw:
  extensions:
    items:
      - name: dds_extension
        path: ${DDS_LIB}
        options:
          domain_id: ${DOMAIN_ID}
          transport_policy: shm_only
  logging:
    core_level: INFO
    backends:
      - type: console
  scheduling:
    executors:
${EXECUTOR_BLOCK}
  messaging:
    drivers:
      - type: dds
        options:
          domain_id: ${DOMAIN_ID}
          ros2_compatible: false
          publishers:
            - topic: "${QOS_TOPIC_PATTERN}"
              qos:
                history: keep_last
                depth: 8192
                reliability: reliable
                durability: volatile
          subscribers:
            - topic: "${QOS_TOPIC_PATTERN}"
              qos:
                history: keep_last
                depth: 8192
                reliability: reliable
                durability: volatile
            - topic: "${TOPIC}"
              executor: work_thread_pool
              loan_mode: async
    subscribers:
      - topic: "${TOPIC}"
        enable_drivers: [dds]
  modules:
    modules:
      - name: DdsSendSubscriberModule
        log_level: INFO
DdsSendSubscriberModule:
  bench_options:
    payload_bytes: ${PAYLOAD_BYTES}
    hz: ${HZ}
    duration_sec: ${DURATION_SEC}
    message_count: ${MESSAGE_COUNT}
    start_delay_ms: ${START_DELAY_MS}
    label: ${LABEL}
    warmup_messages: ${WARMUP_MESSAGES}
    csv_path: ${CSV_PATH}
EOF

rm -f "$CSV_PATH"

"${RUN_PREFIX[@]}" "$SUB_APP" "$SUB_CFG" >"$SUB_LOG" 2>&1 &
SUB_PID=$!
sleep 1
if ! kill -0 "$SUB_PID" 2>/dev/null; then
  wait "$SUB_PID" || sub_status=$?
  echo "FAIL: subscriber exited early with status ${sub_status:-0}" >&2
  cat "$SUB_LOG" >&2
  exit 1
fi

"${RUN_PREFIX[@]}" "$PUB_APP" "$PUB_CFG" >"$PUB_LOG" 2>&1 &
PUB_PID=$!

if [[ "$MESSAGE_COUNT" -gt 0 ]]; then
  max_wait=$((MESSAGE_COUNT / HZ + 15))
  if [[ "$max_wait" -lt 15 ]]; then max_wait=15; fi
  for ((ii = 0; ii < max_wait; ++ii)); do
    if grep -qE "PublisherModule done: total_seq=${MESSAGE_COUNT} mode=fixed_count target_count=${MESSAGE_COUNT}" "$PUB_LOG"; then
      break
    fi
    if ! kill -0 "$PUB_PID" 2>/dev/null; then
      wait "$PUB_PID" || pub_status=$?
      echo "FAIL: publisher exited before fixed-count completion with status ${pub_status:-0}" >&2
      cat "$PUB_LOG" >&2
      exit 1
    fi
    sleep 1
  done
  if ! grep -qE "PublisherModule done: total_seq=${MESSAGE_COUNT} mode=fixed_count target_count=${MESSAGE_COUNT}" "$PUB_LOG"; then
    echo "FAIL: publisher did not complete fixed-count target ${MESSAGE_COUNT}" >&2
    cat "$PUB_LOG" >&2
    exit 1
  fi
  # Give the subscriber/executor path a short grace window to drain the exact
  # fixed-count send before finalizing its CSV on shutdown.
  sleep 3
else
  sleep $((DURATION_SEC + 4))
fi

if [[ -n "${PUB_PID:-}" ]]; then
  terminate_process "$PUB_PID"
  unset PUB_PID
fi
terminate_process "$SUB_PID"
unset SUB_PID
trap - EXIT

require_log() {
  local pattern="$1"
  local file="$2"
  if ! grep -qE "$pattern" "$file"; then
    echo "FAIL: missing pattern '$pattern' in $file" >&2
    echo "--- $file ---" >&2
    cat "$file" >&2
    exit 1
  fi
}

reject_log() {
  local pattern="$1"
  local file="$2"
  if grep -qE "$pattern" "$file"; then
    echo "FAIL: unexpected pattern '$pattern' in $file" >&2
    echo "--- $file ---" >&2
    cat "$file" >&2
    exit 1
  fi
}

validate_csv_row() {
  python3 - "$CSV_PATH" "$LABEL" "$MESSAGE_COUNT" "$WARMUP_MESSAGES" <<'PY'
import csv
from pathlib import Path
import sys

path = Path(sys.argv[1])
label = sys.argv[2]
message_count = int(sys.argv[3])
warmup_messages = int(sys.argv[4])
if not path.exists():
    print(f"FAIL: CSV not created: {path}", file=sys.stderr)
    sys.exit(1)

with path.open(newline="") as f:
    rows = list(csv.DictReader(f))

matches = [row for row in rows if row.get("label") == label]
if not matches:
    print(f"FAIL: missing CSV row for {label}", file=sys.stderr)
    sys.exit(1)

row = matches[-1]
try:
    count = int(float(row.get("count", "0")))
except ValueError as exc:
    print(f"FAIL: invalid count for {label}: {exc}: {row}", file=sys.stderr)
    sys.exit(1)

if count <= 0:
    print(f"FAIL: zero received samples for {label}: {row}", file=sys.stderr)
    sys.exit(1)

if message_count > 0:
    expected = max(message_count - warmup_messages, 0)
    if count != expected:
        print(
            f"FAIL: fixed-count row mismatch for {label}: count={count}, expected={expected}: {row}",
            file=sys.stderr,
        )
        sys.exit(1)

print(f"PASS: {label} count={count}")
PY
}

require_log "Using executor 'work_thread_pool' for topic '${TOPIC}'" "$SUB_LOG"
require_log "loan_mode=ASYNC" "$SUB_LOG"
if [[ "$MESSAGE_COUNT" -gt 0 ]]; then
  EXPECTED_RECEIVED=$((MESSAGE_COUNT - WARMUP_MESSAGES))
  if [[ "$EXPECTED_RECEIVED" -lt 0 ]]; then EXPECTED_RECEIVED=0; fi
  require_log "SubscriberModule Stop: received=${EXPECTED_RECEIVED} .*target_count=${MESSAGE_COUNT}" "$SUB_LOG"
  require_log "PublisherModule done: total_seq=${MESSAGE_COUNT} mode=fixed_count target_count=${MESSAGE_COUNT}" "$PUB_LOG"
else
  require_log "SubscriberModule Stop: received=[1-9][0-9]*" "$SUB_LOG"
  require_log "PublisherModule done: total_seq=[1-9][0-9]*" "$PUB_LOG"
fi
reject_log "Invalid DDS subscriber executor" "$SUB_LOG"
validate_csv_row

echo "PASS: DDS send SHM async executor E2E (${DDS_EXECUTOR_CASE})"
echo "  csv: $CSV_PATH"
echo "  publisher log: $PUB_LOG"
echo "  subscriber log: $SUB_LOG"
if [[ -n "${CPUSET}" ]]; then
  echo "  cpuset: ${CPUSET}"
fi
