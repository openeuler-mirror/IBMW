#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Two-process latency / CPU comparison of the IBMW transport benchmark lanes:
#   iox_send      iceoryx (RouDi) via ibmw::transport::Send()
#   shm_send      the self-developed shm transport (shm_ext), same modules
#   dds_send_shm  Fast DDS SHM transport (reference, needs a DDS build)
# or, with LANES, the zero-copy loan lanes:
#   iox_loan_v2 / shm_loan_v2        POD loan (LoanedSample<T>), iceoryx vs shm
#   iox_loaned_ctx / shm_loaned_ctx  ctx-meta loan (CDR serialized into the chunk)
# This is a benchmark, not a test: it prints medians per lane and payload,
# marks runs that produced no samples as NO SAMPLES (e.g. iox_send at 4MB with
# the default RouDi pool) and always exits 0.
#
# Build (once): configure IBMW with IBMW_BUILD_EXAMPLES=ON IBMW_BUILD_BENCHMARKS=ON
# IBMW_BUILD_ICEORYX_EXTENSION=ON IBMW_BUILD_SHM_EXTENSION=ON (and
# IBMW_BUILD_DDS_EXTENSION=ON for dds_send_shm), then build the targets
#   iox_send_{publisher,subscriber}_app shm_send_{publisher,subscriber}_app iox-roudi
#   ibmw_samples_cpp_transport_benchmark_{iox,shm}_send_build_all
#   (loan lanes: {iox,shm}_loan_v2_*_app iox_loaned_ctx_*_app shm_loaned_ctx_*_app and their _build_all)
#   (dds_send_shm_{publisher,subscriber}_app ibmw_samples_cpp_transport_benchmark_dds_send_shm_build_all)
#
# Usage: bench_transport_lanes.sh [latency] [cpu]        (default: both)
#
# Environment:
#   BUILD_DIR      build with iox_send/shm_send          (required)
#   DDS_BUILD_DIR  build with dds_send_shm               (default: none, lane skipped)
#   OUT_DIR        CSVs and report                       (default: $BUILD_DIR/bench_lanes)
#   LANES          lanes to run                          (default: iox_send shm_send [dds_send_shm])
#   PAYLOADS       payload bytes (1K..4M bench sizes)    (default: 1024 4096 16384 65536 262144 1048576 4194304)
#   ROUNDS         rounds, lane order alternates          (default: 3)
#   DURATION_SEC   seconds per latency run at 100 Hz     (default: 20)
#   PIN_CPUS       taskset cpu list for every run        (default: none)
#   CPU_PAYLOADS   payloads for the cpu stage            (default: 4096 1048576)
#   CPU_WARM/CPU_WINDOW  cpu sampling start/length, s     (default: 6 / 20)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BENCH_SRC="${REPO_DIR}/src/samples/cpp/transport_benchmark"
: "${BUILD_DIR:?set BUILD_DIR to an IBMW build with the benchmark lanes}"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
DDS_BUILD_DIR="${DDS_BUILD_DIR:-}"
OUT_DIR="${OUT_DIR:-${BUILD_DIR}/bench_lanes}"
DEFAULT_LANES="iox_send shm_send"
[[ -n "${DDS_BUILD_DIR}" ]] && DEFAULT_LANES+=" dds_send_shm"
read -r -a LANES <<<"${LANES:-${DEFAULT_LANES}}"
read -r -a PAYLOADS <<<"${PAYLOADS:-1024 4096 16384 65536 262144 1048576 4194304}"
read -r -a CPU_PAYLOADS <<<"${CPU_PAYLOADS:-4096 1048576}"
ROUNDS="${ROUNDS:-3}"
DURATION_SEC="${DURATION_SEC:-20}"
CPU_WARM="${CPU_WARM:-6}"
CPU_WINDOW="${CPU_WINDOW:-20}"
# `env` as a no-op prefix keeps the command line valid without pinning.
PIN=(env)
[[ -n "${PIN_CPUS:-}" ]] && PIN=(taskset -c "${PIN_CPUS}")

mkdir -p "${OUT_DIR}/runs"
log() { printf '\n==== [bench] %s ====\n' "$*"; }

# run_latency <lane> <payload> <csv>: one fixed-rate run, appends one CSV row.
run_latency() {
  local lane=$1 payload=$2 csv=$3
  rm -f "${csv}"
  case "${lane}" in
    dds_send_shm)
      (cd "${DDS_BUILD_DIR}" && timeout 120 "${PIN[@]}" env BUILD_DIR="${DDS_BUILD_DIR}" PAYLOAD_BYTES="${payload}" \
          HZ=100 DURATION_SEC="${DURATION_SEC}" CSV_PATH="${csv}" \
          bash "${BENCH_SRC}/dds_send_shm/validate_dds_send_shm_executor_e2e.sh") ;;
    *)
      (cd "${BUILD_DIR}" && "${PIN[@]}" env BUILD_DIR="${BUILD_DIR}" PAYLOAD_BYTES="${payload}" \
          DURATION_SEC="${DURATION_SEC}" RUN_TIMEOUT_SEC=$((DURATION_SEC + 40)) CSV_PATH="${csv}" \
          bash "${BENCH_SRC}/validate_transport_bench_matrix.sh" "${lane}") ;;
  esac >"${csv%.csv}.log" 2>&1
}

latency_stage() {
  local round payload lane order csv
  log "latency: lanes=${LANES[*]} payloads=${PAYLOADS[*]} rounds=${ROUNDS} ${DURATION_SEC}s@100Hz"
  for ((round = 1; round <= ROUNDS; ++round)); do
    for payload in "${PAYLOADS[@]}"; do
      order=("${LANES[@]}")
      if ((round % 2 == 0)); then  # alternate order so drift does not favour one lane
        order=(); for ((i = ${#LANES[@]} - 1; i >= 0; --i)); do order+=("${LANES[i]}"); done
      fi
      for lane in "${order[@]}"; do
        csv="${OUT_DIR}/runs/r${round}_${lane}_${payload}.csv"
        if run_latency "${lane}" "${payload}" "${csv}" && [[ -s "${csv}" ]]; then
          printf '  r%d %-13s %8d  %s\n' "${round}" "${lane}" "${payload}" "$(tail -1 "${csv}" | awk -F, '{printf "%s samples, p50=%sus p99=%sus", $2, $6, $8}')"
        else
          printf '  r%d %-13s %8d  NO SAMPLES (see %s)\n' "${round}" "${lane}" "${payload}" "${csv%.csv}.log"
        fi
      done
    done
  done
  python3 - "${OUT_DIR}/runs" <<'PY' | tee "${OUT_DIR}/latency.md"
import csv, glob, re, statistics, sys
from collections import defaultdict
rows = defaultdict(list)
for f in glob.glob(sys.argv[1] + '/*.csv'):
    for r in csv.DictReader(open(f)):
        m = re.match(r'(.*?)_(?:strict|executor_e2e_[a-z_]+?)_(\d+)$', r['label'])
        if m and float(r['count']) > 0:
            rows[(m.group(1), int(m.group(2)))].append(r)
keys = ['p50_us', 'p90_us', 'p99_us', 'p999_us', 'max_us']
print('| payload | lane | rounds | samples/round | ' + ' | '.join(k[:-3] + ' (us)' for k in keys) + ' |')
print('|---|---|---|---|' + '---|' * len(keys))
for lane, p in sorted(rows, key=lambda k: (k[1], k[0])):
    rs = rows[(lane, p)]
    med = {k: statistics.median(float(r[k]) for r in rs) for k in keys}
    size = f'{p >> 20}MB' if p >= 1 << 20 else f'{p >> 10}KB'
    cnt = int(statistics.median(float(r['count']) for r in rs))
    print(f'| {size} | {lane} | {len(rs)} | {cnt} | ' + ' | '.join(f'{med[k]:.1f}' for k in keys) + ' |')
PY
}

# Steady-state CPU of one lane: utime+stime of every lane process (and RouDi)
# between CPU_WARM and CPU_WARM+CPU_WINDOW, as % of one core.
cpu_probe() {
  local lane=$1 payload=$2 dir=${BUILD_DIR} role t0 t1 s0 s1 n runner
  [[ "${lane}" == dds_send_shm ]] && return 0  # the DDS lane has its own e2e launcher
  # Match on the command line: the engine renames threads, including main's comm.
  local pattern="/(iox-roudi|${lane}_(publisher|subscriber)_app)( |$)"
  for role in publisher subscriber; do
    # A backup left by an interrupted run is the pristine file: restore it
    # instead of backing up the patched one.
    [[ -f "${OUT_DIR}/${lane}_${role}.yaml.bak" ]] &&
      mv "${OUT_DIR}/${lane}_${role}.yaml.bak" "${dir}/cfg/${lane}/${role}.yaml"
    cp "${dir}/cfg/${lane}/${role}.yaml" "${OUT_DIR}/${lane}_${role}.yaml.bak"
    CPU_RESTORE+=("${OUT_DIR}/${lane}_${role}.yaml.bak:${dir}/cfg/${lane}/${role}.yaml")
    sed -i -E "s/^(\s*)payload_bytes:.*/\1payload_bytes: ${payload}/; s/^(\s*)duration_sec:.*/\1duration_sec: $((CPU_WARM + CPU_WINDOW + 10))/" \
      "${dir}/cfg/${lane}/${role}.yaml"
  done
  ticks() { pgrep -f -- "${pattern}" | while read -r pid; do awk '{print $14 + $15}' "/proc/${pid}/stat" 2>/dev/null; done |
              awk '{s += $1} END {print s + 0}'; }
  (cd "${dir}" && DURATION=$((CPU_WARM + CPU_WINDOW + 5)) "${PIN[@]}" "./start_${lane}.sh") >"${OUT_DIR}/cpu_${lane}_${payload}.log" 2>&1 &
  runner=$!
  CPU_RUNNER=${runner}
  sleep "${CPU_WARM}"
  t0=$(ticks); s0=$(date +%s.%N)
  sleep "${CPU_WINDOW}"
  t1=$(ticks); s1=$(date +%s.%N)
  n=$(pgrep -fc -- "${pattern}")
  wait "${runner}" 2>/dev/null
  CPU_RUNNER=""
  restore_cpu_configs
  awk -v a="${t0}" -v b="${t1}" -v hz="$(getconf CLK_TCK)" -v s0="${s0}" -v s1="${s1}" -v l="${lane}" -v p="${payload}" -v n="${n}" \
    'BEGIN {printf "%s,%d,%d,%.2f\n", l, p, n, 100 * (b - a) / hz / (s1 - s0)}'
}

# Undo cpu_probe's config patch and stop its lane, also on Ctrl-C / kill.
CPU_RESTORE=()
CPU_RUNNER=""
restore_cpu_configs() {
  local item
  if [[ -n "${CPU_RUNNER}" ]]; then
    # The runner is a subshell; signal its start_<lane>.sh too, whose EXIT
    # trap stops the publisher/subscriber (and RouDi).
    pkill -TERM -P "${CPU_RUNNER}" 2>/dev/null
    kill -TERM "${CPU_RUNNER}" 2>/dev/null
    wait "${CPU_RUNNER}" 2>/dev/null
    CPU_RUNNER=""
  fi
  for item in "${CPU_RESTORE[@]}"; do
    [[ -f "${item%%:*}" ]] && mv "${item%%:*}" "${item#*:}"
  done
  CPU_RESTORE=()
}
trap restore_cpu_configs EXIT
trap 'exit 130' INT TERM

cpu_stage() {
  local round payload lane
  log "cpu: lanes=${LANES[*]} payloads=${CPU_PAYLOADS[*]} rounds=${ROUNDS} window=${CPU_WINDOW}s@100Hz"
  echo "lane,payload,processes,cpu_pct_of_one_core" >"${OUT_DIR}/cpu.csv"
  for ((round = 1; round <= ROUNDS; ++round)); do
    for payload in "${CPU_PAYLOADS[@]}"; do
      for lane in "${LANES[@]}"; do cpu_probe "${lane}" "${payload}" | tee -a "${OUT_DIR}/cpu.csv"; done
    done
  done
}

STAGES=("$@")
[[ ${#STAGES[@]} -eq 0 ]] && STAGES=(latency cpu)
for s in "${STAGES[@]}"; do
  case "${s}" in
    latency) latency_stage ;;
    cpu) cpu_stage ;;
    *) echo "unknown stage: ${s}"; exit 64 ;;
  esac
done
log "results in ${OUT_DIR}"
