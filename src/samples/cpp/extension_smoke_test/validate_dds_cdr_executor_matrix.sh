#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-$(pwd)}"
CFG_DIR="${CFG_DIR:-${BUILD_DIR}/cfg}"
TMP_DIR="${TMP_DIR:-/tmp/ibmw_dds_cdr_executor_matrix}"
RUN_TIMEOUT_SEC="${RUN_TIMEOUT_SEC:-12}"

APP="${BUILD_DIR}/dds_native_app"
BASE_CFG="${CFG_DIR}/dds_native_cfg.yaml"
WORK_DIR="${TMP_DIR}/dds_native_cdr_executor"

mkdir -p "${WORK_DIR}"

if [[ ! -x "${APP}" ]]; then
  echo "missing executable: ${APP}" >&2
  exit 2
fi
if [[ ! -f "${BASE_CFG}" ]]; then
  echo "missing config: ${BASE_CFG}" >&2
  exit 2
fi

write_cfg() {
  local case_name="$1"
  local executor_block="$2"
  local cfg="$3"

  python3 - "${BASE_CFG}" "${cfg}" "${executor_block}" <<'PY'
import pathlib
import sys

src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
executor_block = sys.argv[3]
text = src.read_text()
executor_needle = (
    '      - name: work_executor\n'
    '        type: thread_pool\n'
    '        options:\n'
    '          thread_num: 2\n'
)
needle = '          subscribers:\n            - topic: "(.*)"\n              qos:\n'
replacement = (
    '          subscribers:\n'
    '            - topic: "(.*)"\n'
    '              executor: work_executor\n'
    '              loan_mode: async\n'
    '              qos:\n'
)
if executor_needle not in text:
    raise SystemExit("failed to locate DDS executor block in dds_native_cfg.yaml")
if needle not in text:
    raise SystemExit("failed to locate DDS subscriber rule in dds_native_cfg.yaml")
text = text.replace(executor_needle, executor_block + "\n", 1)
dst.write_text(text.replace(needle, replacement, 1))
PY
}

has_log() {
  local pattern="$1"
  grep -Eq "${pattern}" "${LOG}" 2>/dev/null
}

cleanup() {
  if [[ -n "${APP_PID:-}" ]]; then
    kill -INT "${APP_PID}" 2>/dev/null || true
    sleep 0.5
    kill -KILL "${APP_PID}" 2>/dev/null || true
    wait "${APP_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

run_case() {
  local case_name="$1"
  local executor_block="$2"
  local cfg="${WORK_DIR}/dds_native_cdr_${case_name}.yaml"
  LOG="${WORK_DIR}/dds_native_cdr_${case_name}.log"

  write_cfg "${case_name}" "${executor_block}" "${cfg}"

  pushd "${BUILD_DIR}" >/dev/null
  "${APP}" "${cfg}" >"${LOG}" 2>&1 &
  APP_PID=$!
  popd >/dev/null

  deadline=$((SECONDS + RUN_TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if has_log "Using executor 'work_executor' for topic 'hello_topic'" && \
       has_log "Subscriber registered: topic='hello_topic'.*loan_mode=ASYNC" && \
       has_log "Published DDS message #[0-9]+" && \
       has_log "Received DDS message: id=[0-9]+, message='Hello from ibmw DDS!'"; then
      echo "PASS: DDS CDR async executor path (${case_name})"
      echo "  log: ${LOG}"
      cleanup
      unset APP_PID
      return 0
    fi
    if ! kill -0 "${APP_PID}" 2>/dev/null; then
      echo "dds_native_app exited before strict markers were observed (${case_name})" >&2
      sed -n '1,220p' "${LOG}" >&2
      exit 1
    fi
    sleep 0.2
  done

  echo "timeout waiting for DDS CDR async executor markers in ${LOG}" >&2
  sed -n '1,220p' "${LOG}" >&2
  exit 1
}

run_case "thread_pool" "      - name: work_executor
        type: thread_pool
        options:
          thread_num: 2"

run_case "strand" "      - name: dds_cdr_strand_base_pool
        type: thread_pool
        options:
          thread_num: 2
      - name: work_executor
        type: strand
        options:
          bind_thread_pool_executor_name: dds_cdr_strand_base_pool"

run_case "dedicated" "      - name: work_executor
        type: dedicated"

run_case "parallel" "      - name: work_executor
        type: parallel
        options:
          thread_num: 2"

trap - EXIT
