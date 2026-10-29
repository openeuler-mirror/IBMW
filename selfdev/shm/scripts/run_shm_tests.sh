#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Build and test selfdev shm (zero-copy shared memory transport).
# CI-friendly: every stage builds into its own directory, writes JUnit XML
# to $OUT_DIR, and the script exits non-zero if any stage fails.
#
# Usage: run_shm_tests.sh [stage ...]
#   unit     Release build, all tests once                (default)
#   debug    Debug build, all tests once
#   asan     AddressSanitizer + LeakSanitizer
#   ubsan    UndefinedBehaviorSanitizer (abort on error)
#   tsan     ThreadSanitizer
#   repeat   Release build, all tests REPEAT times (default 20), shuffled
#   stress   Release build, concurrency/stress cases REPEAT times
#   bench    Release build, run ibmw_shm_bench (no pass/fail, prints report)
#   all      unit debug asan ubsan tsan repeat bench
#
# Environment:
#   BUILD_ROOT   build directories      (default: <repo>/build-selfdev-shm)
#   OUT_DIR      JUnit XML + logs        (default: $BUILD_ROOT/reports)
#   GTEST_SRC    local googletest source (default: auto-detect, else download)
#   REPEAT       repeat count            (default: 20)
#   JOBS         build parallelism       (default: nproc)
#   GTEST_FILTER extra gtest filter      (default: *)
#   TEST_TIMEOUT seconds per test stage   (default: 1800; a hang fails the stage)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHM_DIR="$(dirname "${SCRIPT_DIR}")"
SELFDEV_DIR="$(dirname "${SHM_DIR}")"
REPO_DIR="$(dirname "${SELFDEV_DIR}")"
BUILD_ROOT="${BUILD_ROOT:-${REPO_DIR}/build-selfdev-shm}"
OUT_DIR="${OUT_DIR:-${BUILD_ROOT}/reports}"
REPEAT="${REPEAT:-20}"
JOBS="${JOBS:-$(nproc)}"
GTEST_FILTER="${GTEST_FILTER:-*}"
TEST_TIMEOUT="${TEST_TIMEOUT:-1800}"
STRESS_FILTER="*Concurrent*:*Stress*:*Lossless*:*ManyBlocked*:*CrossProcess*"

if [[ -z "${GTEST_SRC:-}" ]]; then
  for c in "${REPO_DIR}/../deps/googletest-src" "${REPO_DIR}/deps/googletest-src" "${REPO_DIR}/build/_deps/googletest-src"; do
    if [[ -f "${c}/CMakeLists.txt" ]]; then GTEST_SRC="$(cd "${c}" && pwd)"; break; fi
  done
fi
GTEST_ARG=()
[[ -n "${GTEST_SRC:-}" ]] && GTEST_ARG=("-Dgoogletest_LOCAL_SOURCE=${GTEST_SRC}")

mkdir -p "${OUT_DIR}"
declare -A RESULT
FAILED=0

log() { printf '\n==== [shm] %s ====\n' "$*"; }

# build <name> <build-type> <extra cxx flags> <extra linker flags>
build() {
  local name=$1 type=$2 cxx=$3 ld=$4 dir="${BUILD_ROOT}/$1"
  log "configure+build ${name} (${type} ${cxx})"
  cmake -S "${SELFDEV_DIR}" -B "${dir}" -DCMAKE_BUILD_TYPE="${type}" -DIBMW_BUILD_TESTS=ON \
        -DIBMW_SELFDEV_SHM=ON -DIBMW_SELFDEV_SHM_BENCH=ON "${GTEST_ARG[@]}" \
        -DCMAKE_CXX_FLAGS="${cxx}" -DCMAKE_EXE_LINKER_FLAGS="${ld}" >"${OUT_DIR}/${name}.configure.log" 2>&1 &&
  cmake --build "${dir}" -j "${JOBS}" >"${OUT_DIR}/${name}.build.log" 2>&1
  local rc=$?
  if [[ ${rc} -ne 0 ]]; then
    tail -40 "${OUT_DIR}/${name}.build.log"
    echo "build ${name} FAILED (see ${OUT_DIR}/${name}.build.log)"
  fi
  return ${rc}
}

# run_tests <stage> <build name> <gtest args...>
run_tests() {
  local stage=$1 name=$2; shift 2
  local bin="${BUILD_ROOT}/${name}/shm/ibmw_selfdev_shm_test"
  log "test ${stage}"
  timeout --signal=KILL "${TEST_TIMEOUT}" "${bin}" --gtest_filter="${GTEST_FILTER}" --gtest_output="xml:${OUT_DIR}/${stage}.xml" "$@" \
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
    unit)   build release Release "" "" && run_tests unit release || rc=1 ;;
    debug)  build debug Debug "" "" && run_tests debug debug || rc=1 ;;
    asan)   ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:halt_on_error=1" \
              build asan RelWithDebInfo "-fsanitize=address -fno-omit-frame-pointer" "-fsanitize=address" &&
            ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" run_tests asan asan || rc=1 ;;
    ubsan)  build ubsan RelWithDebInfo "-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
              "-fsanitize=undefined" &&
            UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" run_tests ubsan ubsan || rc=1 ;;
    tsan)   build tsan RelWithDebInfo "-fsanitize=thread -fno-omit-frame-pointer" "-fsanitize=thread" &&
            TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" run_tests tsan tsan || rc=1 ;;
    repeat) build release Release "" "" &&
            run_tests repeat release --gtest_repeat="${REPEAT}" --gtest_shuffle || rc=1 ;;
    stress) build release Release "" "" &&
            GTEST_FILTER="${STRESS_FILTER}" run_tests stress release --gtest_repeat="${REPEAT}" || rc=1 ;;
    bench)  build release Release "" "" && log "bench" &&
            "${BUILD_ROOT}/release/shm/ibmw_shm_bench" | tee "${OUT_DIR}/bench.txt" || rc=1 ;;
    *) echo "unknown stage: ${s}"; exit 64 ;;
  esac
  RESULT[${s}]=$([[ ${rc} -eq 0 ]] && echo PASS || echo FAIL)
  [[ ${rc} -eq 0 ]] || FAILED=1
}

STAGES=("$@")
[[ ${#STAGES[@]} -eq 0 ]] && STAGES=(unit)
[[ "${STAGES[*]}" == "all" ]] && STAGES=(unit debug asan ubsan tsan repeat bench)
ORDER=()
for s in "${STAGES[@]}"; do stage "${s}"; ORDER+=("${s}"); done

log "summary (reports in ${OUT_DIR})"
for s in "${ORDER[@]}"; do printf '  %-8s %s\n' "${s}" "${RESULT[${s}]}"; done
exit ${FAILED}
