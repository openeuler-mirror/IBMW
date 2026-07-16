#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Minimal native ROS2 vs IBMW transport benchmark harness.
#
# This intentionally stays outside scripts/run_bench_sweep.sh so the canonical
# transport matrix remains unchanged. The first comparator is dds_send_shm: both
# sides use a regular publish/subscribe path and append rows with the standard
# transport-benchmark CSV schema.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${IBMW_BUILD_DIR:-${REPO_ROOT}/build}"
CSV_PATH="${IBMW_BENCH_CSV:-/tmp/ibmw_bench/ros2_vs_ibmw.csv}"
PAYLOAD_BYTES="${PAYLOAD_BYTES:-4096}"
SIZE_LABEL="${SIZE_LABEL:-4KB}"
MSG_COUNT="${MSG_COUNT:-1000}"
HZ="${HZ:-100}"
WARMUP_MESSAGES="${WARMUP_MESSAGES:-0}"
ROS2_WARMUP_MESSAGES="${ROS2_WARMUP_MESSAGES:-${WARMUP_MESSAGES}}"
START_DELAY_MS="${START_DELAY_MS:-2000}"
IBMW_SUB_EXECUTOR="${IBMW_SUB_EXECUTOR:-work_thread_pool}"
IBMW_LOAN_MODE="${IBMW_LOAN_MODE:-async}"
IBMW_DDS_QOS_DEPTH="${IBMW_DDS_QOS_DEPTH:-256}"
IBMW_DDS_QOS_RELIABILITY="${IBMW_DDS_QOS_RELIABILITY:-reliable}"
IBMW_EXECUTOR_THREADS="${IBMW_EXECUTOR_THREADS:-}"
IBMW_DDS_TRANSPORT_POLICY="${IBMW_DDS_TRANSPORT_POLICY:-}"
ROS2_EXECUTOR="${ROS2_EXECUTOR:-multi}"
ROS2_THREADS="${ROS2_THREADS:-4}"
ROS2_RELIABILITY="${ROS2_RELIABILITY:-${IBMW_DDS_QOS_RELIABILITY}}"
ROS2_TOPIC="${ROS2_TOPIC:-native_ros2_vs_ibmw_benchmark_topic}"
ROS2_WAIT_AFTER_SEND_MS="${ROS2_WAIT_AFTER_SEND_MS:-2000}"
ROS2_WAIT_FOR_MATCH_MS="${ROS2_WAIT_FOR_MATCH_MS:-10000}"
ROS2_LISTEN_MS="${ROS2_LISTEN_MS:-20000}"
ROS2_VS_IBMW_CONTINUE_ON_IBMW_FAIL="${ROS2_VS_IBMW_CONTINUE_ON_IBMW_FAIL:-0}"
RESET_CSV=0
ROS2_SUB_PID=""

usage() {
  sed -n '3,29p' "${BASH_SOURCE[0]}"
  cat <<'USAGE'

Usage:
  scripts/run_ros2_vs_ibmw_bench.sh [--reset-csv] [BYTES LABEL]

Environment:
  IBMW_BUILD_DIR="<ibmw-build-dir>"
  IBMW_BENCH_CSV=/tmp/ibmw_bench/ros2_vs_ibmw.csv
  MSG_COUNT=1000 HZ=100 WARMUP_MESSAGES=0 ROS2_WARMUP_MESSAGES=0 START_DELAY_MS=2000
  IBMW_SUB_EXECUTOR=work_thread_pool IBMW_LOAN_MODE=async IBMW_DDS_QOS_DEPTH=256
  IBMW_DDS_QOS_RELIABILITY=reliable IBMW_EXECUTOR_THREADS=<empty> IBMW_DDS_TRANSPORT_POLICY=<yaml default>
  ROS2_EXECUTOR=multi ROS2_THREADS=4 ROS2_RELIABILITY=${IBMW_DDS_QOS_RELIABILITY}
  ROS2_VS_IBMW_CONTINUE_ON_IBMW_FAIL=0  # diagnostic: still run ROS2 if IBMW count fails
USAGE
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    --reset-csv) RESET_CSV=1; shift ;;
    *)
      PAYLOAD_BYTES="$1"; shift
      SIZE_LABEL="${1:?label required after payload bytes}"; shift
      ;;
  esac
done

if [[ -z "${AMENT_PREFIX_PATH:-}" && -f /opt/ros/humble/setup.bash ]]; then
  set +u
  # shellcheck disable=SC1091
  source /opt/ros/humble/setup.bash
  set -u
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "error: build dir not found: ${BUILD_DIR}" >&2
  exit 2
fi

export LD_LIBRARY_PATH="${BUILD_DIR}:${BUILD_DIR}/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

IBMW_START="${BUILD_DIR}/start_dds_send_shm.sh"
ROS2_BIN="${BUILD_DIR}/native_ros2_benchmark_app"
IBMW_PUB_CFG="${BUILD_DIR}/cfg/dds_send_shm/publisher.yaml"
IBMW_SUB_CFG="${BUILD_DIR}/cfg/dds_send_shm/subscriber.yaml"

for f in "${IBMW_START}" "${ROS2_BIN}" "${IBMW_PUB_CFG}" "${IBMW_SUB_CFG}"; do
  if [[ ! -e "${f}" ]]; then
    echo "error: missing required file: ${f}" >&2
    exit 2
  fi
done

mkdir -p "$(dirname "${CSV_PATH}")"
if (( RESET_CSV )); then
  rm -f "${CSV_PATH}"
  echo "[ros2-vs-ibmw] CSV reset: ${CSV_PATH}"
fi

backup_yaml() {
  cp "${IBMW_PUB_CFG}" "${IBMW_PUB_CFG}.ros2_vs_ibmw_bak"
  cp "${IBMW_SUB_CFG}" "${IBMW_SUB_CFG}.ros2_vs_ibmw_bak"
}

restore_yaml() {
  [[ -f "${IBMW_PUB_CFG}.ros2_vs_ibmw_bak" ]] && \
    mv "${IBMW_PUB_CFG}.ros2_vs_ibmw_bak" "${IBMW_PUB_CFG}"
  [[ -f "${IBMW_SUB_CFG}.ros2_vs_ibmw_bak" ]] && \
    mv "${IBMW_SUB_CFG}.ros2_vs_ibmw_bak" "${IBMW_SUB_CFG}"
}

cleanup() {
  if [[ -n "${ROS2_SUB_PID}" ]]; then
    kill -INT "${ROS2_SUB_PID}" 2>/dev/null || true
    sleep 0.5
    kill -KILL "${ROS2_SUB_PID}" 2>/dev/null || true
  fi
  restore_yaml
}
trap cleanup EXIT
trap 'exit 130' INT TERM

patch_ibmw_yaml() {
  local ibmw_total_count=$(( MSG_COUNT + WARMUP_MESSAGES ))
  python3 - \
    "${IBMW_PUB_CFG}" "${IBMW_SUB_CFG}" \
    "${PAYLOAD_BYTES}" "${HZ}" "${ibmw_total_count}" "${START_DELAY_MS}" \
    "dds_send_shm_${SIZE_LABEL}" "${WARMUP_MESSAGES}" "${CSV_PATH}" \
    "${IBMW_SUB_EXECUTOR}" "${IBMW_LOAN_MODE}" "${IBMW_DDS_QOS_DEPTH}" \
    "${IBMW_DDS_QOS_RELIABILITY}" "${IBMW_EXECUTOR_THREADS}" \
    "${IBMW_DDS_TRANSPORT_POLICY}" <<'PY'
import sys
import yaml

(
    pub_cfg,
    sub_cfg,
    payload_bytes,
    hz,
    message_count,
    start_delay_ms,
    label,
    warmup_messages,
    csv_path,
    ibmw_sub_executor,
    ibmw_loan_mode,
    ibmw_dds_qos_depth,
    ibmw_dds_qos_reliability,
    ibmw_executor_threads,
    ibmw_dds_transport_policy,
) = sys.argv[1:]


def load_yaml(path):
    with open(path) as f:
        return yaml.safe_load(f)


def dump_yaml(path, data):
    # These build-tree configs are transient and restored by the EXIT trap;
    # PyYAML intentionally does not preserve source comments during patching.
    with open(path, "w") as f:
        yaml.safe_dump(data, f, sort_keys=False)


def patch_bench_options(data):
    module_name = next(k for k in data if k.endswith("Module"))
    opts = data[module_name]["bench_options"]
    opts["payload_bytes"] = int(payload_bytes)
    opts["hz"] = int(hz)
    # Fixed-count mode stops on message_count; duration_sec is only a hard upper bound.
    opts["duration_sec"] = 60
    opts["message_count"] = int(message_count)
    opts["start_delay_ms"] = int(start_delay_ms)
    opts["label"] = label
    opts["warmup_messages"] = int(warmup_messages)
    opts["csv_path"] = csv_path


def dds_options(data):
    for driver in data["ibmw"]["messaging"].get("drivers", []):
        if driver.get("type") == "dds":
            return driver.setdefault("options", {})
    raise RuntimeError("missing dds driver options")


def extension_options(data):
    for extension in data.get("ibmw", {}).get("extensions", {}).get("items", []):
        if extension.get("name") == "dds_extension":
            return extension.setdefault("options", {})
    raise RuntimeError("missing dds extension options")


def apply_qos(entry):
    qos = entry.setdefault("qos", {})
    qos["history"] = "keep_last"
    qos["depth"] = int(ibmw_dds_qos_depth)
    qos["reliability"] = ibmw_dds_qos_reliability


def patch_executor_threads(data):
    if not ibmw_executor_threads:
        return
    for executor in data.get("ibmw", {}).get("scheduling", {}).get("executors", []):
        if executor.get("name") == ibmw_sub_executor:
            executor.setdefault("options", {})["thread_num"] = int(ibmw_executor_threads)


def patch_transport_policy(data):
    if ibmw_dds_transport_policy:
        extension_options(data)["transport_policy"] = ibmw_dds_transport_policy


def patch_pub_driver(data):
    opts = dds_options(data)
    pubs = opts.setdefault("publishers", [])
    entry = pubs[0] if pubs else {"topic": "dds_bench_send_shm_topic"}
    entry["topic"] = "dds_bench_send_shm_topic"
    apply_qos(entry)
    if not pubs:
        pubs.append(entry)


def patch_sub_driver(data):
    opts = dds_options(data)
    subs = opts.setdefault("subscribers", [])
    entry = subs[0] if subs else {"topic": "dds_bench_send_shm_topic"}
    entry["topic"] = "dds_bench_send_shm_topic"
    apply_qos(entry)
    if ibmw_sub_executor:
        entry["executor"] = ibmw_sub_executor
    if ibmw_loan_mode:
        entry["loan_mode"] = ibmw_loan_mode
    if not subs:
        subs.append(entry)


pub = load_yaml(pub_cfg)
sub = load_yaml(sub_cfg)
patch_bench_options(pub)
patch_bench_options(sub)
patch_executor_threads(pub)
patch_executor_threads(sub)
patch_transport_policy(pub)
patch_transport_policy(sub)
patch_pub_driver(pub)
patch_sub_driver(sub)
dump_yaml(pub_cfg, pub)
dump_yaml(sub_cfg, sub)
PY
}

clean_shm() {
  rm -f /dev/shm/fastdds_* /dev/shm/fast_datasharing_* \
        /dev/shm/fastrtps_* /dev/shm/sem.fastrtps_* \
        /dev/shm/iceoryx_* /dev/shm/iox_* 2>/dev/null || true
}

validate_latest_row() {
  local row_label="$1"
  python3 - "${CSV_PATH}" "${row_label}" "${MSG_COUNT}" <<'PY'
import csv
import sys

path, label, expected_count = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r.get("label") == label]
except FileNotFoundError:
    print(f"missing csv: {path}", file=sys.stderr)
    sys.exit(2)
if not rows:
    print(f"missing csv row: {label}", file=sys.stderr)
    sys.exit(2)
try:
    count = int(float(rows[-1].get("count", "0")))
except ValueError:
    count = 0
if count <= 0:
    print(f"zero-count csv row: {label}", file=sys.stderr)
    sys.exit(1)
if count < expected_count:
    print(
        f"short csv row: {label} count={count} expected>={expected_count}",
        file=sys.stderr,
    )
    sys.exit(1)
sys.exit(0)
PY
}

run_ibmw() {
  echo "[ros2-vs-ibmw] running dds_send_shm_${SIZE_LABEL}"
  backup_yaml
  patch_ibmw_yaml
  clean_shm
  local duration=$(( (MSG_COUNT + WARMUP_MESSAGES + HZ - 1) / HZ + START_DELAY_MS / 1000 + 8 ))
  (cd "${BUILD_DIR}" && DURATION="${duration}" ./start_dds_send_shm.sh) \
    >"/tmp/ibmw_ros2_vs_ibmw_dds_send_shm_${SIZE_LABEL}.log" 2>&1
  local validate_status=0
  validate_latest_row "dds_send_shm_${SIZE_LABEL}"
  validate_status=$?
  clean_shm
  return "${validate_status}"
}

run_ros2_native() {
  echo "[ros2-vs-ibmw] running native_ros2_${SIZE_LABEL}"
  clean_shm
  "${ROS2_BIN}" \
    --role sub \
    --executor "${ROS2_EXECUTOR}" \
    --threads "${ROS2_THREADS}" \
    --msg-size "${PAYLOAD_BYTES}" \
    --msg-count "${MSG_COUNT}" \
    --frequency "${HZ}" \
    --warmup-count "${ROS2_WARMUP_MESSAGES}" \
    --reliability "${ROS2_RELIABILITY}" \
    --topic "${ROS2_TOPIC}" \
    --listen-ms "${ROS2_LISTEN_MS}" \
    --label "native_ros2_${SIZE_LABEL}" \
    --csv-path "${CSV_PATH}" \
    >"/tmp/ibmw_ros2_vs_ibmw_native_ros2_sub_${SIZE_LABEL}.log" 2>&1 &
  ROS2_SUB_PID=$!
  sleep 2
  "${ROS2_BIN}" \
    --role pub \
    --executor "${ROS2_EXECUTOR}" \
    --threads "${ROS2_THREADS}" \
    --msg-size "${PAYLOAD_BYTES}" \
    --msg-count "${MSG_COUNT}" \
    --frequency "${HZ}" \
    --warmup-count "${ROS2_WARMUP_MESSAGES}" \
    --reliability "${ROS2_RELIABILITY}" \
    --topic "${ROS2_TOPIC}" \
    --wait-after-send-ms "${ROS2_WAIT_AFTER_SEND_MS}" \
    --wait-for-match-ms "${ROS2_WAIT_FOR_MATCH_MS}" \
    --label "native_ros2_${SIZE_LABEL}" \
    >"/tmp/ibmw_ros2_vs_ibmw_native_ros2_pub_${SIZE_LABEL}.log" 2>&1
  wait "${ROS2_SUB_PID}"
  ROS2_SUB_PID=""
  validate_latest_row "native_ros2_${SIZE_LABEL}"
}

IBMW_STATUS=0
ROS2_STATUS=0

set +e
run_ibmw
IBMW_STATUS=$?
set -e

if (( IBMW_STATUS != 0 )); then
  echo "[ros2-vs-ibmw] dds_send_shm_${SIZE_LABEL} failed with status ${IBMW_STATUS}" >&2
  if [[ "${ROS2_VS_IBMW_CONTINUE_ON_IBMW_FAIL}" != "1" ]]; then
    exit "${IBMW_STATUS}"
  fi
  echo "[ros2-vs-ibmw] diagnostic mode: continuing to native ROS2 lane" >&2
fi

set +e
run_ros2_native
ROS2_STATUS=$?
set -e

if (( ROS2_STATUS != 0 )); then
  echo "[ros2-vs-ibmw] native_ros2_${SIZE_LABEL} failed with status ${ROS2_STATUS}" >&2
  exit "${ROS2_STATUS}"
fi

if (( IBMW_STATUS != 0 )); then
  echo "[ros2-vs-ibmw] DIAGNOSTIC complete with IBMW failure: ${CSV_PATH}" >&2
  exit "${IBMW_STATUS}"
fi

echo "[ros2-vs-ibmw] PASS: ${CSV_PATH}"
