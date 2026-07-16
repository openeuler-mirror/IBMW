#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$(pwd)}"
TMP_DIR="${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-dds-loaned}"
mkdir -p "$TMP_DIR"

DOMAIN_ID="${DOMAIN_ID:-42}"
PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}"
HZ="${HZ:-200}"
DURATION_SEC="${DURATION_SEC:-2}"
WARMUP_MESSAGES="${WARMUP_MESSAGES:-0}"
DDS_EXECUTOR_CASE="${DDS_EXECUTOR_CASE:-thread_pool}"

PUB_APP="${PUB_APP:-${BUILD_DIR}/dds_loaned_publisher_app}"
SUB_APP="${SUB_APP:-${BUILD_DIR}/dds_loaned_subscriber_app}"
DDS_LIB="${DDS_LIB:-${BUILD_DIR}/libibmw_dds_transport.so}"

PUB_CFG="${TMP_DIR}/ibmw_dds_loaned_e2e_${DDS_EXECUTOR_CASE}_publisher_${DOMAIN_ID}.yaml"
SUB_CFG="${TMP_DIR}/ibmw_dds_loaned_e2e_${DDS_EXECUTOR_CASE}_subscriber_${DOMAIN_ID}.yaml"
PUB_LOG="${TMP_DIR}/ibmw_dds_loaned_e2e_${DDS_EXECUTOR_CASE}_publisher_${DOMAIN_ID}.log"
SUB_LOG="${TMP_DIR}/ibmw_dds_loaned_e2e_${DDS_EXECUTOR_CASE}_subscriber_${DOMAIN_ID}.log"

case "${DDS_EXECUTOR_CASE}" in
  thread_pool)
    EXECUTOR_BLOCK="      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2"
    ;;
  strand)
    EXECUTOR_BLOCK="      - name: dds_loaned_strand_base_pool
        type: thread_pool
        options:
          thread_num: 2
      - name: work_thread_pool
        type: strand
        options:
          bind_thread_pool_executor_name: dds_loaned_strand_base_pool"
    ;;
  dedicated)
    EXECUTOR_BLOCK="      - name: work_thread_pool
        type: dedicated"
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

stop_process() {
  local pid="$1"
  local name="$2"

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
      echo "FAIL: ${name} did not stop after SIGTERM" >&2
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      exit 1
    fi
  fi

  local status=0
  wait "$pid" || status=$?
  if [[ "$status" -ne 0 && "$status" -ne 143 ]]; then
    echo "FAIL: ${name} exited with status ${status}" >&2
    exit 1
  fi
}

wait_for_exit() {
  local pid="$1"
  local name="$2"
  local timeout_sec="$3"

  for ((i = 0; i < timeout_sec; ++i)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      local status=0
      wait "$pid" || status=$?
      if [[ "$status" -ne 0 ]]; then
        echo "FAIL: ${name} exited with status ${status}" >&2
        exit 1
      fi
      return 0
    fi
    local stat
    stat="$(ps -p "$pid" -o stat= 2>/dev/null || true)"
    if [[ "$stat" == Z* ]]; then
      local status=0
      wait "$pid" || status=$?
      if [[ "$status" -ne 0 ]]; then
        echo "FAIL: ${name} exited with status ${status}" >&2
        exit 1
      fi
      return 0
    fi
    sleep 1
  done

  echo "FAIL: ${name} did not exit within ${timeout_sec}s" >&2
  cleanup
  exit 1
}

cat >"$PUB_CFG" <<EOF
ibmw:
  extensions:
    items:
      - name: dds_extension
        path: ${DDS_LIB}
        options:
          domain_id: ${DOMAIN_ID}
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
          enable_data_sharing: true
          publishers:
            - topic: ".*BenchPlainPayload4KB"
              qos:
                history: keep_last
                depth: 8192
                reliability: reliable
                durability: volatile
    publishers:
      - topic: "dds_bench_loaned_topic"
        enable_drivers: [dds]
  modules:
    modules:
      - name: DdsLoanedPublisherModule
        log_level: INFO
DdsLoanedPublisherModule:
  bench_options:
    payload_bytes: ${PAYLOAD_BYTES}
    hz: ${HZ}
    duration_sec: ${DURATION_SEC}
    label: dds_loaned_e2e_${DDS_EXECUTOR_CASE}_${PAYLOAD_BYTES}
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
          enable_data_sharing: true
          subscribers:
            - topic: "dds_bench_loaned_topic"
              executor: work_thread_pool
              loan_mode: async
            - topic: ".*BenchPlainPayload4KB"
              qos:
                history: keep_last
                depth: 8192
                reliability: reliable
                durability: volatile
    subscribers:
      - topic: "dds_bench_loaned_topic"
        enable_drivers: [dds]
  modules:
    modules:
      - name: DdsLoanedSubscriberModule
        log_level: INFO
DdsLoanedSubscriberModule:
  bench_options:
    payload_bytes: ${PAYLOAD_BYTES}
    hz: ${HZ}
    duration_sec: ${DURATION_SEC}
    label: dds_loaned_e2e_${DDS_EXECUTOR_CASE}_${PAYLOAD_BYTES}
    warmup_messages: ${WARMUP_MESSAGES}
    csv_path: ""
EOF

"$SUB_APP" "$SUB_CFG" >"$SUB_LOG" 2>&1 &
SUB_PID=$!
sleep 1
if ! kill -0 "$SUB_PID" 2>/dev/null; then
  wait "$SUB_PID" || sub_status=$?
  echo "FAIL: subscriber exited early with status ${sub_status:-0}" >&2
  cat "$SUB_LOG" >&2
  exit 1
fi

"$PUB_APP" "$PUB_CFG" >"$PUB_LOG" 2>&1 &
PUB_PID=$!
sleep $((DURATION_SEC + 4))

kill -KILL "$PUB_PID" 2>/dev/null || true
wait "$PUB_PID" 2>/dev/null || true
unset PUB_PID
kill -KILL "$SUB_PID" 2>/dev/null || true
wait "$SUB_PID" 2>/dev/null || true
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

require_log "Using executor 'work_thread_pool' for topic 'dds_bench_loaned_topic'" "$SUB_LOG"
require_log "loan_mode=ASYNC" "$SUB_LOG"
require_log "SubscriberModule first sample: payload_bytes=${PAYLOAD_BYTES} seq=[0-9]+ received=[1-9][0-9]*" "$SUB_LOG"
require_log "can_loan=true" "$PUB_LOG"
require_log "PublisherModule done: total_seq=[1-9][0-9]* loan_failed=0 publish_failed=0 fallback=0" "$PUB_LOG"
reject_log "not zero-copy capable" "$PUB_LOG"
reject_log "Invalid DDS subscriber executor" "$SUB_LOG"

echo "PASS: DDS loaned async executor E2E (${DDS_EXECUTOR_CASE})"
echo "  publisher log: $PUB_LOG"
echo "  subscriber log: $SUB_LOG"
