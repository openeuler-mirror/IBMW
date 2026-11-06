#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Build-switch checks for the self-developed shm transport (shm_ext): the
# switch is optional and independent of iceoryx.
#
#   bench-only  IBMW_BUILD_SHM_EXTENSION=ON, IBMW_BUILD_ICEORYX_EXTENSION=OFF,
#               IBMW_BUILD_BENCHMARKS=ON: everything builds, there is no
#               iceoryx target or library, and the four shm_* benchmark lanes deliver
#               the expected samples (shm_send, shm_loan_v2, shm_loaned_pod,
#               shm_loaned_ctx) and exit cleanly.
#   coexist     both extensions ON in one build:
#                 - everything builds; the only strong symbol they both export
#                   is the plugin entry IbmwExtEntryV2;
#                 - the EXT tests pass;
#                 - shm_extension with driver_name iceoryx next to
#                   iceoryx_extension is refused at start-up (EXT-61, end to end);
#                 - shm_extension (driver_name shm) and iceoryx_extension loaded
#                   in the same processes, both drivers configured (with RouDi),
#                   carry shm_send traffic over shm and shut down cleanly.
#   preset      the "shm" CMake preset configures with shm ON and iceoryx OFF
#               and builds the plugin (binary dir redirected under BUILD_ROOT).
#   all         bench-only coexist preset (default)
#
# Both stages need Fast CDR/Fast DDS (the benchmark payloads are CDR) and, for
# coexist, the iceoryx sources; pass offline sources and package locations in
# IBMW_CMAKE_ARGS, e.g.
#   IBMW_CMAKE_ARGS="-Dfastcdr_DIR=... -Dfastrtps_DIR=... -DFETCHCONTENT_SOURCE_DIR_ICEORYX=..."
#
# Environment:
#   BUILD_ROOT       build directories and logs (default: <repo>/build-shm-switch)
#   JOBS             parallel build jobs (default: nproc)
#   DURATION_SEC     publish time per run (default: 3)
#   IBMW_CMAKE_ARGS  extra CMake arguments (last, may override)
#
# The applications must find their shared libraries at run time. With a system
# Fast DDS (e.g. /opt/ros/humble) export LD_LIBRARY_PATH=/opt/ros/humble/lib (or
# source its setup.bash): libfastrtps carries no runpath for libfastcdr. Each
# stage checks this with ldd before running and fails if a library is missing.
#
# The applications and tests use a private IBMW_SHM_ROOT under /tmp, so the shm
# side leaves nothing in /dev/shm. Processes are started and stopped by PID
# only, and only while they are still children of this script. The coexist
# stage starts its own RouDi, which uses /dev/shm/iceoryx_*; it refuses to run
# if another RouDi or its management segment exists, and stops RouDi
# with SIGINT (SIGKILL only after 10 s) so RouDi removes its segments.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${REPO_DIR}/build-shm-switch}"
JOBS="${JOBS:-$(nproc)}"
DURATION_SEC="${DURATION_SEC:-3}"
read -r -a EXTRA_ARGS <<<"${IBMW_CMAKE_ARGS:-}"
LANES=(shm_send shm_loan_v2 shm_loaned_pod shm_loaned_ctx)
COMMON_ARGS=(
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DIBMW_BUILD_RUNTIME=ON -DIBMW_BUILD_EXAMPLES=ON
  -DIBMW_BUILD_BENCHMARKS=ON -DIBMW_BUILD_TESTS=ON -DIBMW_BUILD_SHM_EXTENSION=ON
  -DIBMW_BUILD_DDS_EXTENSION=OFF -DIBMW_BUILD_ROS2_TYPES=OFF -DIBMW_BUILD_WITH_ROS2=OFF
  -DIBMW_BUILD_LOANED_MESSAGE_DEMO=OFF -DIBMW_BUILD_CLI_TOOLS=OFF
)

# Private shm service root (short: root + "/ibmw_shm.<uid>.<domain>" must fit
# in 63 bytes), removed on exit.
SHM_ROOT="$(mktemp -d /tmp/ibsw.XXXXXX)" || exit 1
export IBMW_SHM_ROOT="${SHM_ROOT}"

declare -A RESULT
PIDS=()
ROUDI_PID=""
log() { echo "==== [shm-switch] $* ===="; }
fail() { echo "FAIL: $*" >&2; return 1; }

# own <pid>: the process is still running as a child of this script (a PID
# that was reaped may already belong to someone else).
own() {
  local stat
  [[ -n "$1" ]] && stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
  stat="${stat##*) }"
  [[ "${stat%% *}" != Z && "$(cut -d' ' -f2 <<<"${stat}")" == "$$" ]]
}
signal_own() {  # signal_own <signal> <pid...>
  local sig=$1 p; shift
  for p in "$@"; do own "${p}" && kill "-${sig}" "${p}" 2>/dev/null; done
  return 0
}
wait_gone() {  # wait_gone <tenths of a second> <pid...>: 0 once none is running
  local n=$1 i p alive; shift
  for ((i = 0; i < n; i++)); do
    alive=0
    for p in "$@"; do own "${p}" && alive=1; done
    [[ ${alive} -eq 0 ]] && return 0
    sleep 0.1
  done
  return 1
}
stop_roudi() {
  [[ -n "${ROUDI_PID}" ]] || return 0
  signal_own INT "${ROUDI_PID}"
  wait_gone 100 "${ROUDI_PID}" || { echo "RouDi did not stop in 10 s; killing it" >&2; signal_own KILL "${ROUDI_PID}"; }
  wait "${ROUDI_PID}" 2>/dev/null
  ROUDI_PID=""
}
cleanup() {
  signal_own INT "${PIDS[@]:-}"
  wait_gone 50 "${PIDS[@]:-}" || signal_own KILL "${PIDS[@]:-}"
  PIDS=()
  stop_roudi
  rm -rf "${SHM_ROOT}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

configure_build() {  # configure_build <dir> <iceoryx ON|OFF> <targets...>
  local dir=$1 iox=$2; shift 2
  cmake -S "${REPO_DIR}" -B "${dir}" "${COMMON_ARGS[@]}" -DIBMW_BUILD_ICEORYX_EXTENSION="${iox}" \
    "${EXTRA_ARGS[@]}" >"${dir}.configure.log" 2>&1 || { tail -30 "${dir}.configure.log"; fail "configure ${dir}"; return 1; }
  grep -q '^IBMW_BUILD_BENCHMARKS:BOOL=ON$' "${dir}/CMakeCache.txt" || { fail "IBMW_BUILD_BENCHMARKS was turned off"; return 1; }
  cmake --build "${dir}" -j "${JOBS}" --target "$@" >"${dir}.build.log" 2>&1 ||
    { grep -m20 -E 'error|Error' "${dir}.build.log"; fail "build ${dir}"; return 1; }
}

lane_targets() {
  local l
  for l in "${LANES[@]}"; do
    echo "${l}_publisher_app" "${l}_subscriber_app" "ibmw_samples_cpp_transport_benchmark_${l}_build_all"
  done
}

# patch_cfg <src> <dst> <label> <csv> <domain> [conflict|coexist]
patch_cfg() {
  python3 - "$@" "${DURATION_SEC}" <<'PY'
import re, sys
args = sys.argv[1:]
duration = args.pop()
src, dst, label, csv, domain = args[:5]
mode = args[5] if len(args) > 5 else ""
text = open(src).read()
def sub(pattern, repl, count=0):
    global text
    new, n = re.subn(pattern, repl, text, count=count, flags=re.M)
    if n == 0:
        sys.exit(f"patch_cfg: no match for {pattern!r} in {src}")
    text = new
sub(r"^(\s*)duration_sec:.*$", rf"\g<1>duration_sec: {duration}")
sub(r"^(\s*)label:.*$", rf"\g<1>label: {label}")
if "csv_path:" in text:
    sub(r"^(\s*)csv_path:.*$", rf"\g<1>csv_path: {csv}")
sub(r"^(\s*)driver_name: iceoryx$", rf'\g<1>driver_name: iceoryx\n\g<1>domain: "{domain}"', count=1)
if mode in ("conflict", "coexist"):
    m = re.search(r'^(\s*)runtime_id: "([^"]+)"$', text, flags=re.M)
    if not m:
        sys.exit("patch_cfg: no runtime_id")
    item = (f'{m.group(0)}\n      - name: iceoryx_extension\n        path: ./libibmw_iceoryx_transport.so\n'
            f'        options:\n          runtime_id: "{m.group(2)}_iox{domain}"')
    text = text.replace(m.group(0), item, 1)
if mode == "coexist":
    # Both drivers configured (iceoryx_extension is only shut down cleanly when
    # its driver is in use); the topic goes over shm. The listener options that
    # follow belong to the shm driver, as in the original shm configuration.
    sub(r"^(\s*)driver_name: iceoryx$", r"\g<1>driver_name: shm")
    sub(r"^(\s*)- type: iceoryx$", r"\g<1>- type: iceoryx\n\g<1>- type: shm", count=1)
    sub(r"enable_drivers: \[iceoryx\]", "enable_drivers: [shm]")
open(dst, "w").write(text)
PY
}

# run_pair <build> <lane> <pub cfg> <sub cfg> <run dir>: subscriber first, then
# the publisher; both get SIGINT after DURATION_SEC + 3 s. Fails unless both
# exit with status 0 within 5 s and log no Fatal/Error line.
run_pair() {
  local build=$1 lane=$2 pub_cfg=$3 sub_cfg=$4 run=$5 sub pub st rc=0 role
  (cd "${build}" && exec "./${lane}_subscriber_app" "${sub_cfg}") >"${run}/${lane}.sub.log" 2>&1 &
  sub=$!; PIDS+=("${sub}")
  sleep 1
  (cd "${build}" && exec "./${lane}_publisher_app" "${pub_cfg}") >"${run}/${lane}.pub.log" 2>&1 &
  pub=$!; PIDS+=("${pub}")
  sleep $((DURATION_SEC + 3))
  signal_own INT "${pub}" "${sub}"
  wait_gone 50 "${pub}" "${sub}" || { fail "${lane}: did not exit within 5 s of SIGINT"; rc=1; signal_own KILL "${pub}" "${sub}"; }
  wait "${pub}"; st=$?
  [[ ${st} -eq 0 ]] || { fail "${lane} publisher exited with ${st}"; rc=1; }
  wait "${sub}"; st=$?
  [[ ${st} -eq 0 ]] || { fail "${lane} subscriber exited with ${st}"; rc=1; }
  PIDS=()
  for role in pub sub; do
    # IBMW lines (console colors stripped) and iceoryx's own "[Error]"/"[Fatal]".
    if sed 's/\x1b\[[0-9;]*m//g' "${run}/${lane}.${role}.log" | grep -E '^(Fatal|Error) |\[ ?(Fatal|Error) ?\]'; then
      fail "${lane} ${role}: Fatal/Error in ${run}/${lane}.${role}.log"; rc=1
    fi
  done
  return "${rc}"
}

check_libs() {  # check_libs <binary...>: every NEEDED library resolves
  local b missing
  for b in "$@"; do
    missing="$(ldd "${b}" 2>&1 | grep 'not found')"
    [[ -z "${missing}" ]] || { echo "${missing}"; fail "$(basename "${b}"): unresolved libraries (set LD_LIBRARY_PATH)"; return 1; }
  done
}

# check_csv <csv> <label> <publisher cfg>: at least 90 % of the samples the
# publisher sends after warm-up (hz * duration_sec - warmup_messages).
check_csv() {
  python3 - "$1" "$2" "$3" <<'PY'
import csv, re, sys
path, label, cfg = sys.argv[1:]
text = open(cfg).read()
def num(key):
    m = re.search(rf"^\s*{key}:\s*([0-9.]+)", text, flags=re.M)
    if not m:
        sys.exit(f"FAIL: {label}: no {key} in {cfg}")
    return float(m.group(1))
expected = num("hz") * num("duration_sec") - num("warmup_messages")
minimum = int(expected * 0.9)
try:
    rows = [r for r in csv.DictReader(open(path, newline="")) if r.get("label") == label]
except FileNotFoundError:
    sys.exit(f"FAIL: {label}: no CSV {path}")
if not rows:
    sys.exit(f"FAIL: {label}: no CSV row")
count = int(float(rows[-1].get("count", "0")))
if count <= 0 or count < minimum:
    sys.exit(f"FAIL: {label}: count={count}, expected at least {minimum} of {int(expected)}")
print(f"PASS: {label} count={count} (expected {int(expected)}) p50_us={rows[-1].get('p50_us')}")
PY
}

stage_bench_only() {
  local dir="${BUILD_ROOT}/bench-only" run l rc=0
  run="${dir}/switch-run"
  log "bench-only (shm ON, iceoryx OFF, benchmarks ON)"
  mkdir -p "${BUILD_ROOT}"
  # shellcheck disable=SC2046
  configure_build "${dir}" OFF all $(lane_targets) || return 1
  grep -q '^IBMW_BUILD_ICEORYX_EXTENSION:BOOL=OFF$' "${dir}/CMakeCache.txt" || { fail "iceoryx is not off"; return 1; }
  local targets
  targets="$(cmake --build "${dir}" --target help 2>/dev/null)" && [[ -n "${targets}" ]] ||
    { fail "cannot list the build targets"; return 1; }
  if grep -qE '(^|[ :])(iox-roudi|iceoryx_posh|ibmw_extensions_iceoryx_ext)[: ]' <<<"${targets}"; then
    fail "iceoryx targets exist in a shm-only build"; return 1
  fi
  find "${dir}" -name 'libibmw_iceoryx_transport.so*' | grep -q . && { fail "libibmw_iceoryx_transport.so was built"; return 1; }
  echo "no iceoryx targets or libraries"
  for l in "${LANES[@]}"; do
    check_libs "${dir}/${l}_publisher_app" "${dir}/${l}_subscriber_app" || return 1
  done
  rm -rf "${run}"; mkdir -p "${run}"
  for l in "${LANES[@]}"; do
    patch_cfg "${dir}/cfg/${l}/publisher.yaml" "${run}/${l}.pub.yaml" "switch_${l}" "${run}/lat.csv" "swb$$" &&
      patch_cfg "${dir}/cfg/${l}/subscriber.yaml" "${run}/${l}.sub.yaml" "switch_${l}" "${run}/lat.csv" "swb$$" ||
      { rc=1; continue; }
    run_pair "${dir}" "${l}" "${run}/${l}.pub.yaml" "${run}/${l}.sub.yaml" "${run}" || rc=1
    check_csv "${run}/lat.csv" "switch_${l}" "${run}/${l}.pub.yaml" || rc=1
  done
  return "${rc}"
}

stage_coexist() {
  local dir="${BUILD_ROOT}/coexist" run rc=0
  run="${dir}/switch-run"
  log "coexist (shm ON, iceoryx ON)"
  mkdir -p "${BUILD_ROOT}"
  configure_build "${dir}" ON all ibmw_extensions_shm_ext ibmw_extensions_iceoryx_ext ibmw_extensions_shm_ext_test \
    iox-roudi shm_send_publisher_app shm_send_subscriber_app ibmw_samples_cpp_transport_benchmark_shm_send_build_all ||
    return 1

  local a="${dir}/libibmw_shm_transport.so" b="${dir}/libibmw_iceoryx_transport.so" both
  [[ -f "${a}" && -f "${b}" ]] || { fail "missing plugin(s): ${a} ${b}"; return 1; }
  # IBMW_BUILD_TESTS adds --coverage, which links libgcov's runtime (__gcov_*,
  # mangle_path) into every library; those are not plugin symbols.
  strong() { nm -D --defined-only "$1" | awk '$2 ~ /^[TDBR]$/ && $3 !~ /^__gcov_/ && $3 != "mangle_path" {print $3}' | sort -u; }
  both="$(comm -12 <(strong "${a}") <(strong "${b}"))"
  echo "strong symbols exported by both plugins: ${both//$'\n'/ }"
  [[ "${both}" == "IbmwExtEntryV2" ]] || { fail "unexpected shared strong symbols"; rc=1; }

  local test_bin
  test_bin="$(find "${dir}" -type f -name ibmw_extensions_shm_ext_test -perm -u+x | head -1)"
  (cd "$(dirname "${test_bin}")" && "${test_bin}" --gtest_brief=1) >"${run}.ext-test.log" 2>&1 ||
    { tail -20 "${run}.ext-test.log"; fail "EXT tests"; rc=1; }
  grep -E '^\[  PASSED  \]' "${run}.ext-test.log"

  check_libs "${dir}/shm_send_publisher_app" "${dir}/shm_send_subscriber_app" "${dir}/iox-roudi" || return 1
  rm -rf "${run}"; mkdir -p "${run}"
  # Refused: same driver name from both extensions. No RouDi is needed: the
  # shm extension is listed first and fails before iceoryx initializes.
  patch_cfg "${dir}/cfg/shm_send/publisher.yaml" "${run}/conflict.pub.yaml" switch_conflict "${run}/lat.csv" "swc$$" conflict ||
    return 1
  (cd "${dir}" && timeout 30 ./shm_send_publisher_app "${run}/conflict.pub.yaml") >"${run}/conflict.log" 2>&1
  local st=$?
  if [[ ${st} -ne 0 ]] && grep -q "driver_name 'iceoryx' is also registered by extension 'iceoryx_extension'" "${run}/conflict.log"; then
    echo "PASS: conflict refused (exit ${st})"
  else
    tail -20 "${run}/conflict.log"; fail "conflict not refused (exit ${st})"; rc=1
  fi

  # Both loaded, traffic over the shm driver; RouDi serves iceoryx_extension.
  if pgrep -x iox-roudi >/dev/null || compgen -G '/dev/shm/iceoryx_mgmt' >/dev/null ||
    compgen -G '/dev/shm/iox*_management' >/dev/null; then
    fail "another iox-roudi is running (or its /dev/shm management segment exists); not starting a second one"; return 1
  fi
  "${dir}/iox-roudi" -l debug >"${run}/roudi.log" 2>&1 &
  ROUDI_PID=$!
  sleep 2
  own "${ROUDI_PID}" || { cat "${run}/roudi.log"; ROUDI_PID=""; fail "RouDi did not start"; return 1; }
  patch_cfg "${dir}/cfg/shm_send/publisher.yaml" "${run}/coexist.pub.yaml" switch_coexist "${run}/lat.csv" "swc$$" coexist &&
    patch_cfg "${dir}/cfg/shm_send/subscriber.yaml" "${run}/coexist.sub.yaml" switch_coexist "${run}/lat.csv" "swc$$" coexist ||
    { stop_roudi; return 1; }
  run_pair "${dir}" shm_send "${run}/coexist.pub.yaml" "${run}/coexist.sub.yaml" "${run}" || rc=1
  stop_roudi
  if compgen -G '/dev/shm/iceoryx_mgmt' >/dev/null || compgen -G '/dev/shm/iox*_management' >/dev/null; then
    fail "RouDi left its management segment in /dev/shm"; rc=1
  fi
  check_csv "${run}/lat.csv" switch_coexist "${run}/coexist.pub.yaml" || rc=1
  # iceoryx_extension registers its runtime with RouDi in both processes.
  local reg
  reg="$(grep -c "Registered new application shm_send_.*_iox" "${run}/roudi.log")"
  if [[ "${reg}" -ge 2 ]]; then
    echo "PASS: iceoryx_extension registered with RouDi in ${reg} processes"
  else
    tail -20 "${run}/roudi.log"; fail "iceoryx_extension did not register with RouDi (${reg})"; rc=1
  fi
  return "${rc}"
}

stage_preset() {
  local dir="${BUILD_ROOT}/preset"
  log "preset shm"
  mkdir -p "${BUILD_ROOT}"
  (cd "${REPO_DIR}" && cmake --preset shm -B "${dir}" "${EXTRA_ARGS[@]}") >"${dir}.configure.log" 2>&1 ||
    { tail -30 "${dir}.configure.log"; fail "configure preset shm"; return 1; }
  grep -q '^IBMW_BUILD_SHM_EXTENSION:BOOL=ON$' "${dir}/CMakeCache.txt" &&
    grep -q '^IBMW_BUILD_ICEORYX_EXTENSION:BOOL=OFF$' "${dir}/CMakeCache.txt" ||
    { fail "preset shm: expected shm ON, iceoryx OFF"; return 1; }
  cmake --build "${dir}" -j "${JOBS}" --target ibmw_extensions_shm_ext >"${dir}.build.log" 2>&1 ||
    { grep -m20 -E 'error|Error' "${dir}.build.log"; fail "build preset shm"; return 1; }
  [[ -f "${dir}/libibmw_shm_transport.so" ]] || { fail "preset shm: no libibmw_shm_transport.so"; return 1; }
  echo "PASS: preset shm (shm ON, iceoryx OFF, plugin built)"
}

STAGES=("$@")
[[ ${#STAGES[@]} -eq 0 ]] && STAGES=(all)
[[ " ${STAGES[*]} " == *" all "* ]] && STAGES=(bench-only coexist preset)
rc=0
for s in "${STAGES[@]}"; do
  case "${s}" in
    bench-only) stage_bench_only ;;
    coexist) stage_coexist ;;
    preset) stage_preset ;;
    *) echo "unknown stage: ${s}" >&2; exit 2 ;;
  esac
  st=$?
  RESULT[${s}]=$([[ ${st} -eq 0 ]] && echo PASS || echo FAIL)
  [[ ${st} -eq 0 ]] || rc=1
done
log "summary"
for s in "${STAGES[@]}"; do printf '  %-11s %s\n' "${s}" "${RESULT[${s}]}"; done
exit "${rc}"
