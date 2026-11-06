#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Build and test the shm transport extension (src/extensions/transport/shm_ext,
# the iceoryx_ext replacement) inside the full IBMW build. Companion of
# run_shm_tests.sh, which tests the standalone selfdev library.
# CI-friendly: every stage builds only the test target into its own
# directory, writes JUnit XML to $OUT_DIR and the script exits non-zero if
# any stage fails.
#
# Usage: run_shm_ext_tests.sh [stage ...]
#   unit     RelWithDebInfo build, all EXT tests once          (default)
#   repeat   same build, REPEAT times (default 20), shuffled
#   asan     AddressSanitizer + LeakSanitizer
#   tsan     ThreadSanitizer
#   all      unit repeat asan tsan
#
# Environment:
#   BUILD_ROOT       build directories        (default: <repo>/build-shm-ext)
#   OUT_DIR          JUnit XML + logs         (default: $BUILD_ROOT/reports)
#   IBMW_CMAKE_ARGS  extra cmake arguments, e.g. FETCHCONTENT_SOURCE_DIR_*
#                    for offline builds        (default: empty)
#   REPEAT           repeat count             (default: 20)
#   JOBS             build parallelism        (default: nproc)
#   GTEST_FILTER     extra gtest filter       (default: *)
#   TEST_TIMEOUT     seconds per test stage   (default: 1800)
#   TSAN_CC/TSAN_CXX compilers for tsan       (default: newest gcc-12+ found)
#   CMAKE_GENERATOR  cmake generator          (default: Ninja if installed)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${REPO_DIR}/build-shm-ext}"
OUT_DIR="${OUT_DIR:-${BUILD_ROOT}/reports}"
REPEAT="${REPEAT:-20}"
JOBS="${JOBS:-$(nproc)}"
GTEST_FILTER="${GTEST_FILTER:-*}"
TEST_TIMEOUT="${TEST_TIMEOUT:-1800}"
TARGET=ibmw_extensions_shm_ext_test
read -r -a EXTRA_ARGS <<<"${IBMW_CMAKE_ARGS:-}"
# Minimal configuration: runtime + shm_ext only, so CI needs no DDS, iceoryx
# or other third-party SDKs. IBMW_CMAKE_ARGS comes last and may override.
MINIMAL_ARGS=(
  -DIBMW_BUILD_SHM_EXTENSION=ON -DIBMW_BUILD_ICEORYX_EXTENSION=OFF -DIBMW_BUILD_DDS_EXTENSION=OFF
  -DIBMW_BUILD_ECHO_EXTENSION=OFF -DIBMW_BUILD_GRPC_EXTENSION=OFF -DIBMW_BUILD_MQTT_EXTENSION=OFF
  -DIBMW_BUILD_NET_EXTENSION=OFF -DIBMW_BUILD_ZENOH_EXTENSION=OFF -DIBMW_BUILD_PROXY_EXTENSION=OFF
  -DIBMW_BUILD_PARAMETER_EXTENSION=OFF -DIBMW_BUILD_LOG_CONTROL_EXTENSION=OFF
  -DIBMW_BUILD_OPENTELEMETRY_EXTENSION=OFF -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=OFF
  -DIBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION=OFF -DIBMW_BUILD_TIME_MANIPULATOR_EXTENSION=OFF
  -DIBMW_BUILD_TOPIC_LOGGER_EXTENSION=OFF -DIBMW_BUILD_OHOS_CAPTURE_AGENT=OFF
  -DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=OFF -DIBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE=OFF
  -DIBMW_BUILD_CLI_TOOLS=OFF -DIBMW_BUILD_EXAMPLES=OFF -DIBMW_BUILD_PROTOCOLS=OFF -DIBMW_BUILD_ROS2_TYPES=OFF
)

# TSan must intercept pthread_cond_clockwait (used by libstdc++ for
# condition_variable::wait_until on steady_clock); GCC 11's libtsan does not,
# which hides unlocks and yields false "double lock"/race reports. Prefer the
# newest GCC that has it; override with TSAN_CC/TSAN_CXX.
pick_tsan_compiler() {
  local v
  # A single override is paired with its sibling so C and C++ never mix
  # compiler versions (and their libtsan runtimes).
  if [[ -n "${TSAN_CXX:-}" && -z "${TSAN_CC:-}" ]]; then
    case "${TSAN_CXX}" in
      *clang++*) TSAN_CC="${TSAN_CXX/clang++/clang}" ;;
      *g++*)     TSAN_CC="${TSAN_CXX/g++/gcc}" ;;
      *)         TSAN_CC=cc ;;
    esac
  fi
  if [[ -n "${TSAN_CC:-}" && -z "${TSAN_CXX:-}" ]]; then
    case "${TSAN_CC}" in
      *clang*) TSAN_CXX="${TSAN_CC/clang/clang++}" ;;
      *gcc*)   TSAN_CXX="${TSAN_CC/gcc/g++}" ;;
      *)       TSAN_CXX=c++ ;;
    esac
  fi
  [[ -n "${TSAN_CXX:-}" ]] && return
  for v in 14 13 12; do
    if command -v "g++-${v}" >/dev/null && command -v "gcc-${v}" >/dev/null; then
      TSAN_CC="gcc-${v}"; TSAN_CXX="g++-${v}"; return
    fi
  done
  TSAN_CC=cc; TSAN_CXX=c++
  TSAN_FALLBACK=1
}
TSAN_FALLBACK=0
pick_tsan_compiler
# GCC 12+ warns (-Wtsan) that TSan does not model atomic_thread_fence; the
# fences only order futex sleep/wake announcements, so keep it a warning.
TSAN_EXTRA=""
if echo 'int main(){}' | "${TSAN_CXX}" -x c++ -Wno-error=tsan -Werror -fsyntax-only - >/dev/null 2>&1; then
  TSAN_EXTRA="-Wno-error=tsan"
fi

GENERATOR_ARG=()
if [[ -z "${CMAKE_GENERATOR:-}" ]] && command -v ninja >/dev/null; then GENERATOR_ARG=(-G Ninja); fi

mkdir -p "${OUT_DIR}"
declare -A RESULT
FAILED=0

log() { printf '\n==== [shm_ext] %s ====\n' "$*"; }

# build <name> <extra cxx flags> <extra linker flags> [extra cmake args...]
# IBMW instruments test builds with gcov unless ADDRESS_SANITIZER or
# THREAD_SANITIZER is set; gcov counters are not atomic and show up as TSan
# races, so sanitizer stages pass those switches.
build() {
  local name=$1 cxx=$2 ld=$3 dir="${BUILD_ROOT}/$1"
  shift 3
  log "configure+build ${name} (${cxx:-no sanitizer})"
  cmake -S "${REPO_DIR}" -B "${dir}" "${GENERATOR_ARG[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIBMW_BUILD_TESTS=ON \
        "${MINIMAL_ARGS[@]}" "${EXTRA_ARGS[@]}" \
        -DCMAKE_CXX_FLAGS="${cxx}" -DCMAKE_C_FLAGS="${cxx}" \
        -DCMAKE_EXE_LINKER_FLAGS="${ld}" -DCMAKE_SHARED_LINKER_FLAGS="${ld}" "$@" \
        >"${OUT_DIR}/${name}.configure.log" 2>&1 &&
  cmake --build "${dir}" -j "${JOBS}" --target "${TARGET}" >"${OUT_DIR}/${name}.build.log" 2>&1
  local rc=$?
  if [[ ${rc} -ne 0 ]]; then
    tail -40 "${OUT_DIR}/${name}.build.log" 2>/dev/null || tail -40 "${OUT_DIR}/${name}.configure.log"
    echo "build ${name} FAILED (see ${OUT_DIR}/${name}.*.log)"
  fi
  return ${rc}
}

# run_tests <stage> <build name> <gtest args...>
run_tests() {
  local stage=$1 name=$2; shift 2
  local bin
  bin="$(find "${BUILD_ROOT}/${name}" -name "${TARGET}" -type f -executable | head -1)"
  log "test ${stage}"
  if [[ -z "${bin}" ]]; then
    echo "test binary ${TARGET} not found under ${BUILD_ROOT}/${name}"
    return 1
  fi
  timeout --signal=KILL "${TEST_TIMEOUT}" "${bin}" --gtest_filter="${GTEST_FILTER}" \
      --gtest_output="xml:${OUT_DIR}/${stage}.xml" "$@" \
      2>&1 | tee "${OUT_DIR}/${stage}.test.log" | grep -E '^\[ *(FAILED|PASSED) *\]|Failure|Sanitizer|runtime error' | sort | uniq -c
  local rc=${PIPESTATUS[0]}
  [[ ${rc} -eq 137 ]] && echo "stage ${stage} timed out after ${TEST_TIMEOUT}s"
  if grep -qE 'ERROR: (Address|Thread|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error:' "${OUT_DIR}/${stage}.test.log"; then
    echo "sanitizer report found in ${OUT_DIR}/${stage}.test.log"
    rc=1
  fi
  return ${rc}
}

stage() {
  local s=$1 rc=0
  case "${s}" in
    unit)   build release "" "" && run_tests unit release || rc=1 ;;
    repeat) build release "" "" &&
            run_tests repeat release --gtest_repeat="${REPEAT}" --gtest_shuffle || rc=1 ;;
    asan)   build asan "-fsanitize=address -fno-omit-frame-pointer" "-fsanitize=address" -DADDRESS_SANITIZER=ON &&
            ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" run_tests asan asan || rc=1 ;;
    tsan)   [[ ${TSAN_FALLBACK} -eq 1 ]] &&
              echo "warning: no gcc-12+ found, tsan uses cc/c++; GCC 11 libtsan reports false races (set TSAN_CC/TSAN_CXX)"
            build tsan "-fsanitize=thread -fno-omit-frame-pointer ${TSAN_EXTRA}" "-fsanitize=thread" -DTHREAD_SANITIZER=ON \
              -DCMAKE_C_COMPILER="${TSAN_CC}" -DCMAKE_CXX_COMPILER="${TSAN_CXX}" &&
            TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" run_tests tsan tsan || rc=1 ;;
    *) echo "unknown stage: ${s}"; exit 64 ;;
  esac
  RESULT[${s}]=$([[ ${rc} -eq 0 ]] && echo PASS || echo FAIL)
  [[ ${rc} -eq 0 ]] || FAILED=1
}

STAGES=("$@")
[[ ${#STAGES[@]} -eq 0 ]] && STAGES=(unit)
[[ "${STAGES[*]}" == "all" ]] && STAGES=(unit repeat asan tsan)
ORDER=()
for s in "${STAGES[@]}"; do stage "${s}"; ORDER+=("${s}"); done

log "summary (reports in ${OUT_DIR})"
for s in "${ORDER[@]}"; do printf '  %-8s %s\n' "${s}" "${RESULT[${s}]}"; done
exit ${FAILED}
