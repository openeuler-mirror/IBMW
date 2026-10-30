#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
TMP_DIR="${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-transport-bench}"
PAYLOAD_BYTES="${PAYLOAD_BYTES:-4096}"
DURATION_SEC="${DURATION_SEC:-5}"
RUN_TIMEOUT_SEC="${RUN_TIMEOUT_SEC:-25}"
CSV_PATH="${CSV_PATH:-${TMP_DIR}/ibmw_transport_bench_matrix_${PAYLOAD_BYTES}_$$.csv}"
DDS_LOANED_E2E="${DDS_LOANED_E2E:-${BUILD_DIR}/validate_dds_loaned_e2e.sh}"
if [[ ! -x "${DDS_LOANED_E2E}" && -x "${SCRIPT_DIR}/dds_loaned/validate_dds_loaned_e2e.sh" ]]; then
  DDS_LOANED_E2E="${SCRIPT_DIR}/dds_loaned/validate_dds_loaned_e2e.sh"
fi
DDS_SEND_SHM_E2E="${DDS_SEND_SHM_E2E:-${BUILD_DIR}/validate_dds_send_shm_executor_e2e.sh}"
if [[ ! -x "${DDS_SEND_SHM_E2E}" && -x "${SCRIPT_DIR}/dds_send_shm/validate_dds_send_shm_executor_e2e.sh" ]]; then
  DDS_SEND_SHM_E2E="${SCRIPT_DIR}/dds_send_shm/validate_dds_send_shm_executor_e2e.sh"
fi
DDS_SEND_TCP_E2E="${DDS_SEND_TCP_E2E:-${BUILD_DIR}/validate_dds_send_tcp_executor_e2e.sh}"
if [[ ! -x "${DDS_SEND_TCP_E2E}" && -x "${SCRIPT_DIR}/dds_send_tcp/validate_dds_send_tcp_executor_e2e.sh" ]]; then
  DDS_SEND_TCP_E2E="${SCRIPT_DIR}/dds_send_tcp/validate_dds_send_tcp_executor_e2e.sh"
fi

mkdir -p "${TMP_DIR}"

ALL_SAMPLES=(
  iox_loaned_pod
  iox_loaned_ctx
  iox_send
  shm_send
  iox_loan_v2
  shm_loan_v2
  shm_loaned_ctx
  shm_loaned_pod
  dds_loaned
  dds_send_shm
  dds_send_tcp
  net_send_tcp
)

if [[ "$#" -gt 0 ]]; then
  SAMPLES=("$@")
else
  SAMPLES=("${ALL_SAMPLES[@]}")
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "FAIL: build dir not found: ${BUILD_DIR}" >&2
  exit 2
fi

cd "${BUILD_DIR}"

backup_files=()

cleanup() {
  for bak in "${backup_files[@]:-}"; do
    local original="${bak%.matrix_bak}"
    if [[ -f "${bak}" ]]; then
      mv "${bak}" "${original}"
    fi
  done
  clean_runtime_state
}
trap cleanup EXIT

backup_yaml() {
  local sample="$1"
  local role="$2"
  local path="cfg/${sample}/${role}.yaml"
  local bak="${path}.matrix_bak"
  if [[ ! -f "${path}" ]]; then
    echo "FAIL: missing config ${path}" >&2
    exit 2
  fi
  if [[ ! -f "${bak}" ]]; then
    cp "${path}" "${bak}"
    backup_files+=("${bak}")
  fi
}

patch_yaml() {
  local sample="$1"
  local role="$2"
  local duration_sec="${3:-${DURATION_SEC}}"
  local label="${sample}_strict_${PAYLOAD_BYTES}"
  local path="cfg/${sample}/${role}.yaml"

  python3 - "${path}" "${PAYLOAD_BYTES}" "${duration_sec}" "${label}" "${CSV_PATH}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
payload, duration, label, csv_path = sys.argv[2:]
lines = path.read_text().splitlines()
out = []
for line in lines:
    stripped = line.lstrip()
    indent = line[:len(line) - len(stripped)]
    if stripped.startswith("payload_bytes:"):
        out.append(f"{indent}payload_bytes: {payload}")
    elif stripped.startswith("duration_sec:"):
        out.append(f"{indent}duration_sec: {duration}")
    elif stripped.startswith("label:"):
        out.append(f"{indent}label: {label}")
    elif stripped.startswith("csv_path:"):
        out.append(f"{indent}csv_path: {csv_path}")
    else:
        out.append(line)
path.write_text("\n".join(out) + "\n")
PY
}

validate_csv_row() {
  local sample="$1"
  local label="${sample}_strict_${PAYLOAD_BYTES}"
  python3 - "${CSV_PATH}" "${label}" <<'PY'
import csv
from pathlib import Path
import sys

path = Path(sys.argv[1])
label = sys.argv[2]
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
    p99 = float(row.get("p99_us", "0"))
except ValueError as exc:
    print(f"FAIL: invalid CSV row for {label}: {exc}: {row}", file=sys.stderr)
    sys.exit(1)

if count <= 0:
    print(f"FAIL: zero received samples for {label}: {row}", file=sys.stderr)
    sys.exit(1)

print(f"PASS: {label} count={count} p99_us={p99}")
PY
}

run_sample_once() {
  local sample="$1"
  local duration_sec="$2"
  local timeout_sec="$3"
  local log="$4"

  clean_runtime_state
  timeout -s TERM "${timeout_sec}" env DURATION="$((duration_sec + 5))" \
    "./start_${sample}.sh" >"${log}" 2>&1
  local status=$?
  clean_runtime_state
  return "${status}"
}

clean_runtime_state() {
  # The benchmark start scripts are intentionally tiny and force-kill their
  # children after each short run. In a strict matrix, however, a stale RouDi or
  # iceoryx lock from the previous lane can make the next iceoryx lane publish
  # into the wrong runtime and produce zero samples (or trip a publisher crash).
  # Keep this cleanup local to the benchmark process names used by this matrix.
  ps -eo pid=,comm= | awk '$2 ~ /^(iox-roudi|iox_loaned_pod_|iox_loaned_ctx_|iox_send_|shm_send_|iox_loan_v2_|shm_loan_v2_|shm_loaned_ctx_|shm_loaned_pod_|dds_loaned_|dds_send_shm_|dds_send_tcp_|net_send_tcp_)/ {print $1}' | xargs -r kill -TERM 2>/dev/null || true
  sleep 1
  ps -eo pid=,comm= | awk '$2 ~ /^(iox-roudi|iox_loaned_pod_|iox_loaned_ctx_|iox_send_|shm_send_|iox_loan_v2_|shm_loan_v2_|shm_loaned_ctx_|shm_loaned_pod_|dds_loaned_|dds_send_shm_|dds_send_tcp_|net_send_tcp_)/ {print $1}' | xargs -r kill -9 2>/dev/null || true
  rm -rf /dev/shm/fastdds_* /dev/shm/fast_datasharing_* \
         /dev/shm/fastrtps_* /dev/shm/sem.fastrtps_* \
         /dev/shm/iceoryx_* /dev/shm/iox_* /dev/shm/iox-* \
         /tmp/iox* /tmp/iceoryx_* 2>/dev/null || true
  sleep 1
}

rm -f "${CSV_PATH}"
echo "[matrix] build_dir=${BUILD_DIR}"
echo "[matrix] csv=${CSV_PATH}"
echo "[matrix] payload_bytes=${PAYLOAD_BYTES} duration_sec=${DURATION_SEC}"

for sample in "${SAMPLES[@]}"; do
  case " ${ALL_SAMPLES[*]} " in
    *" ${sample} "*) ;;
    *) echo "FAIL: unknown sample '${sample}'" >&2; exit 2 ;;
  esac

  if [[ ! -x "./start_${sample}.sh" ]]; then
    echo "FAIL: missing executable ./start_${sample}.sh" >&2
    exit 2
  fi

  if [[ "${sample}" == "dds_loaned" ]]; then
    echo "[matrix] running dds_loaned via dedicated async-loan E2E..."
    if [[ ! -x "${DDS_LOANED_E2E}" ]]; then
      echo "FAIL: missing dds_loaned E2E script: ${DDS_LOANED_E2E}" >&2
      exit 2
    fi
    clean_runtime_state
    log="${TMP_DIR}/ibmw_transport_bench_matrix_${sample}_$$.log"
    if ! env BUILD_DIR="${BUILD_DIR}" TMP_DIR="${TMP_DIR}" \
        PAYLOAD_BYTES="${PAYLOAD_BYTES}" HZ=100 DURATION_SEC="${DURATION_SEC}" \
        "${DDS_LOANED_E2E}" >"${log}" 2>&1; then
      echo "FAIL: ${sample} dedicated E2E failed; log=${log}" >&2
      exit 1
    fi
    echo "PASS: dds_loaned_strict_${PAYLOAD_BYTES} dedicated async-loan E2E"
    continue
  fi

  if [[ "${sample}" == "dds_send_shm" ]]; then
    echo "[matrix] running dds_send_shm via dedicated async-executor E2E..."
    if [[ ! -x "${DDS_SEND_SHM_E2E}" ]]; then
      echo "FAIL: missing dds_send_shm E2E script: ${DDS_SEND_SHM_E2E}" >&2
      exit 2
    fi
    clean_runtime_state
    log="${TMP_DIR}/ibmw_transport_bench_matrix_${sample}_$$.log"
    if ! env BUILD_DIR="${BUILD_DIR}" TMP_DIR="${TMP_DIR}" \
        PAYLOAD_BYTES="${PAYLOAD_BYTES}" HZ=100 DURATION_SEC="${DURATION_SEC}" \
        "${DDS_SEND_SHM_E2E}" >"${log}" 2>&1; then
      echo "FAIL: ${sample} dedicated E2E failed; log=${log}" >&2
      exit 1
    fi
    echo "PASS: dds_send_shm_strict_${PAYLOAD_BYTES} dedicated async-executor E2E"
    continue
  fi

  if [[ "${sample}" == "dds_send_tcp" ]]; then
    echo "[matrix] running dds_send_tcp via dedicated async-executor E2E..."
    if [[ ! -x "${DDS_SEND_TCP_E2E}" ]]; then
      echo "FAIL: missing dds_send_tcp E2E script: ${DDS_SEND_TCP_E2E}" >&2
      exit 2
    fi
    clean_runtime_state
    log="${TMP_DIR}/ibmw_transport_bench_matrix_${sample}_$$.log"
    dds_send_tcp_duration_sec="${DDS_SEND_TCP_DURATION_SEC:-${DURATION_SEC}}"
    if (( dds_send_tcp_duration_sec < 4 )); then
      dds_send_tcp_duration_sec=4
    fi
    if ! env BUILD_DIR="${BUILD_DIR}" TMP_DIR="${TMP_DIR}" \
        PAYLOAD_BYTES="${PAYLOAD_BYTES}" HZ=100 DURATION_SEC="${dds_send_tcp_duration_sec}" \
        "${DDS_SEND_TCP_E2E}" >"${log}" 2>&1; then
      echo "FAIL: ${sample} dedicated E2E failed; log=${log}" >&2
      exit 1
    fi
    echo "PASS: dds_send_tcp_strict_${PAYLOAD_BYTES} dedicated async-executor E2E"
    continue
  fi

  backup_yaml "${sample}" publisher
  backup_yaml "${sample}" subscriber
  patch_yaml "${sample}" publisher "${DURATION_SEC}"
  patch_yaml "${sample}" subscriber "${DURATION_SEC}"

  echo "[matrix] running ${sample}..."
  log="${TMP_DIR}/ibmw_transport_bench_matrix_${sample}_$$.log"
  if ! run_sample_once "${sample}" "${DURATION_SEC}" "${RUN_TIMEOUT_SEC}" "${log}"; then
    echo "FAIL: ${sample} command failed; log=${log}" >&2
    exit 1
  fi
  if ! validate_csv_row "${sample}"; then
    if [[ "${sample}" == dds_* ]]; then
      echo "[matrix] ${sample} had no samples in the short window; retrying with longer DDS discovery window..."
      rm -f "${CSV_PATH}"
      patch_yaml "${sample}" publisher 8
      patch_yaml "${sample}" subscriber 8
      log="${TMP_DIR}/ibmw_transport_bench_matrix_${sample}_retry_$$.log"
      if ! run_sample_once "${sample}" 8 25 "${log}"; then
        echo "FAIL: ${sample} retry command failed; log=${log}" >&2
        exit 1
      fi
      validate_csv_row "${sample}"
    else
      exit 1
    fi
  fi
done

echo "PASS: transport benchmark strict matrix (${#SAMPLES[@]} samples)"
echo "  csv: ${CSV_PATH}"
