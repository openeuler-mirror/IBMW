#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# IBMW transport-benchmark size-sweep driver.
#
# For each requested payload size, this script:
#   1) Backs up cfg/<sample>/{publisher,subscriber}.yaml under the build tree.
#   2) Patches `payload_bytes`, `label`, and `shm_init_size` in-place.
#   3) Runs the bench samples sequentially via build/start_<sample>.sh. The
#      default list has 10 lanes (iceoryx, self-developed shm, DDS, net);
#      lanes this build does not have (iox_* without iceoryx, shm_* without
#      shm) are skipped. Samples named in IBMW_BENCH_SAMPLES must exist.
#   4) Wipes /dev/shm/{fastdds,fastrtps,fast_datasharing}_* between samples so
#      phantom discovery state from a hard-killed prior run does not silence
#      the next one; /dev/shm/{iceoryx,iox}_* only before iox_* samples.
#   5) Restores yaml from backup on EXIT (trap), even on Ctrl-C.
#
# Latency rows accumulate into ${IBMW_BENCH_CSV} (one row per sample per
# size). Its default is /tmp/ibmw_bench/iox_bench_lat.csv when an iox_* lane
# runs (unchanged for iceoryx builds), else /tmp/ibmw_bench/bench_lat.csv.
# Use --reset-csv to start a fresh sweep.
#
# Default size matrix matches industry DDS/ROS 2 perf reports:
#   1KB / 4KB / 16KB / 64KB / 256KB / 1MB
#
# ----------------------------------------------------------------------------
# Usage:
#   scripts/run_bench_sweep.sh                  # full 6-size sweep
#   scripts/run_bench_sweep.sh --reset-csv      # full sweep, fresh CSV
#   scripts/run_bench_sweep.sh 4096 4KB         # single size run
#   DURATION=15 scripts/run_bench_sweep.sh      # override launcher seconds
#   IBMW_BENCH_RETRIES=3 scripts/run_bench_sweep.sh  # retry failed sample/size runs
#   IBMW_BUILD_DIR="<ibmw-build-dir>" scripts/run_bench_sweep.sh
#   IBMW_BENCH_SAMPLES="dds_loaned dds_send_shm" scripts/run_bench_sweep.sh 4096 4KB
#
# Prerequisites:
#   - cmake --build build && cmake --install build --prefix build
#   - ROS 2 Humble sourced (the script will source it if available)
# ----------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${IBMW_BUILD_DIR:-${REPO_ROOT}/build}"
DURATION="${DURATION:-}"
IBMW_BENCH_RETRIES="${IBMW_BENCH_RETRIES:-1}"

DEFAULT_SAMPLES=(iox_loaned_pod iox_loaned_ctx iox_send
                 shm_loaned_pod shm_loaned_ctx shm_send
                 dds_loaned dds_send_shm dds_send_tcp net_send_tcp)

# Default 6-size matrix: "<bytes> <label>" per entry.
DEFAULT_MATRIX=(
  "1024    1KB"
  "4096    4KB"
  "16384   16KB"
  "65536   64KB"
  "262144  256KB"
  "1048576 1MB"
)

usage() {
  sed -n '3,38p' "${BASH_SOURCE[0]}"
  exit "${1:-0}"
}

# --- arg parsing -------------------------------------------------------------
RESET_CSV=0
SINGLE_SIZE=""
SINGLE_LABEL=""
SWEEP_FAILED=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    --reset-csv) RESET_CSV=1; shift ;;
    *)
      if [[ -z "${SINGLE_SIZE}" ]]; then
        SINGLE_SIZE="$1"; shift
        SINGLE_LABEL="${1:?label required after size}"; shift
      else
        echo "unknown arg: $1" >&2; usage 1
      fi
      ;;
  esac
done

# --- env ---------------------------------------------------------------------
if [[ ! "${IBMW_BENCH_RETRIES}" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: IBMW_BENCH_RETRIES must be a positive integer, got '${IBMW_BENCH_RETRIES}'" >&2
  exit 2
fi

if [[ -z "${AMENT_PREFIX_PATH:-}" && -f /opt/ros/humble/setup.bash ]]; then
  # ROS 2 setup.bash references unbound vars; relax `set -u` for the source.
  set +u
  # shellcheck disable=SC1091
  source /opt/ros/humble/setup.bash
  set -u
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "error: build dir not found: ${BUILD_DIR}" >&2
  echo "       (run cmake --build build && cmake --install build --prefix build first)" >&2
  exit 2
fi
cd "${BUILD_DIR}"

if [[ -n "${IBMW_BENCH_SAMPLES:-}" ]]; then
  # shellcheck disable=SC2206
  SAMPLES=(${IBMW_BENCH_SAMPLES})
  for s in "${SAMPLES[@]}"; do
    if [[ ! -x "./start_${s}.sh" ]]; then
      echo "error: missing build/start_${s}.sh — did you run cmake --install?" >&2
      exit 2
    fi
  done
else
  # A default lane may be missing only when its transport is not built; a
  # missing lane of a built transport is an install error, as before.
  lane_transport_built() {
    local opt
    case "$1" in
      iox_*) opt=IBMW_BUILD_ICEORYX_EXTENSION ;;
      shm_*) opt=IBMW_BUILD_SHM_EXTENSION ;;
      dds_*) opt=IBMW_BUILD_DDS_EXTENSION ;;
      net_*) opt=IBMW_BUILD_NET_EXTENSION ;;
      *) return 0 ;;
    esac
    [[ -f CMakeCache.txt ]] || return 0
    grep -qiE "^${opt}:BOOL=(ON|YES|Y|TRUE|1)$" CMakeCache.txt
  }
  SAMPLES=()
  for s in "${DEFAULT_SAMPLES[@]}"; do
    if [[ -x "./start_${s}.sh" ]]; then
      SAMPLES+=("${s}")
    elif lane_transport_built "${s}"; then
      echo "error: missing build/start_${s}.sh although its transport is built — did you run cmake --install?" >&2
      exit 2
    else
      echo "[sweep] skip ${s}: its transport is not built"
    fi
  done
  if (( ${#SAMPLES[@]} == 0 )); then
    echo "error: no bench sample built in ${BUILD_DIR} — did you run cmake --install?" >&2
    exit 2
  fi
fi

RUN_IOX=0
for s in "${SAMPLES[@]}"; do
  [[ "${s}" == iox_* ]] && RUN_IOX=1
done
if (( RUN_IOX )); then
  CSV_PATH="${IBMW_BENCH_CSV:-/tmp/ibmw_bench/iox_bench_lat.csv}"
else
  CSV_PATH="${IBMW_BENCH_CSV:-/tmp/ibmw_bench/bench_lat.csv}"
fi

mkdir -p "$(dirname "${CSV_PATH}")"
if (( RESET_CSV )); then
  rm -f "${CSV_PATH}"
  echo "[sweep] CSV reset: ${CSV_PATH}"
fi

# --- per-size driver ---------------------------------------------------------
backup_yaml() {
  for s in "${SAMPLES[@]}"; do
    for f in publisher subscriber; do
      local src="cfg/${s}/${f}.yaml"
      local bak="${src}.sweep_bak"
      [[ -f "${bak}" ]] || cp "${src}" "${bak}"
    done
  done
}

restore_yaml() {
  for s in "${SAMPLES[@]}"; do
    for f in publisher subscriber; do
      local bak="cfg/${s}/${f}.yaml.sweep_bak"
      [[ -f "${bak}" ]] && mv "${bak}" "cfg/${s}/${f}.yaml"
    done
  done
}
trap restore_yaml EXIT

patch_yaml() {
  local size="$1" label="$2" shm_init="$3"
  for s in "${SAMPLES[@]}"; do
    for f in publisher subscriber; do
      local y="cfg/${s}/${f}.yaml"
      sed -i -E "s/^([[:space:]]*payload_bytes:[[:space:]]*).*/\1${size}/" "${y}"
      sed -i -E "s/^([[:space:]]*label:[[:space:]]*${s})_[^[:space:]]+/\1_${label}/" "${y}"
      sed -i -E "s/^([[:space:]]*shm_init_size:[[:space:]]*).*/\1${shm_init}/" "${y}"
      sed -i -E "s|^([[:space:]]*csv_path:[[:space:]]*).*|\1${CSV_PATH}|" "${y}"
    done
  done
}

# $1: the sample about to run. iceoryx files are only wiped for iox_* lanes.
clean_shm() {
  rm -f /dev/shm/fastdds_* /dev/shm/fast_datasharing_* \
        /dev/shm/fastrtps_* /dev/shm/sem.fastrtps_* 2>/dev/null || true
  if [[ "$1" == iox_* ]]; then
    rm -f /dev/shm/iceoryx_* /dev/shm/iox_* 2>/dev/null || true
  fi
}

validate_latest_row() {
  local sample="$1" label="$2"
  local row_label="${sample}_${label}"

  python3 - "${CSV_PATH}" "${row_label}" <<'PY'
import csv
import sys

path, label = sys.argv[1], sys.argv[2]
try:
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r.get("label") == label]
except FileNotFoundError:
    print(f"missing csv: {path}", file=sys.stderr)
    sys.exit(2)

if not rows:
    print(f"missing csv row: {label}", file=sys.stderr)
    sys.exit(2)

row = rows[-1]
try:
    count = int(float(row.get("count", "0")))
except ValueError:
    count = 0
if count <= 0:
    print(f"zero-count csv row: {label}", file=sys.stderr)
    sys.exit(1)
sys.exit(0)
PY
}

run_one_size() {
  local size="$1" label="$2"
  local shm_init=$(( size * 2 ))
  (( shm_init < 4096 )) && shm_init=4096

  local duration_label="${DURATION:+${DURATION}s}"
  duration_label="${duration_label:-yaml+grace}"
  echo "=== sweep size=${size} label=${label} duration=${duration_label} shm_init=${shm_init} ==="
  patch_yaml "${size}" "${label}" "${shm_init}"

  for s in "${SAMPLES[@]}"; do
    echo "--- ${s} @ ${label} ---"
    local log="/tmp/sweep_${s}_${label}.log"
    local attempt=1
    local sample_ok=0
    while (( attempt <= IBMW_BENCH_RETRIES )); do
      clean_shm "${s}"
      local status=0
      echo "  attempt ${attempt}/${IBMW_BENCH_RETRIES}"
      if [[ -n "${DURATION}" ]]; then
        DURATION="${DURATION}" "./start_${s}.sh" >"${log}" 2>&1 || status=$?
      else
        "./start_${s}.sh" >"${log}" 2>&1 || status=$?
      fi

      if [[ "${status}" -eq 0 ]]; then
        if validate_latest_row "${s}" "${label}"; then
          echo "  ok"
          sample_ok=1
          break
        else
          echo "  attempt ${attempt}/${IBMW_BENCH_RETRIES} failed: zero-count or missing CSV row (see ${log})"
        fi
      else
        echo "  attempt ${attempt}/${IBMW_BENCH_RETRIES} failed: exit=${status} (see ${log})"
      fi

      (( attempt++ ))
    done

    if (( ! sample_ok )); then
      SWEEP_FAILED=1
    fi
    sleep 1
  done
  echo "=== sweep ${label} done ==="
}

# --- main loop ---------------------------------------------------------------
backup_yaml

if [[ -n "${SINGLE_SIZE}" ]]; then
  run_one_size "${SINGLE_SIZE}" "${SINGLE_LABEL}"
else
  for entry in "${DEFAULT_MATRIX[@]}"; do
    # shellcheck disable=SC2086
    run_one_size ${entry}
  done
fi

echo
echo "[sweep] CSV: ${CSV_PATH}"
echo "[sweep] rows: $(wc -l <"${CSV_PATH}")"
exit "${SWEEP_FAILED}"
