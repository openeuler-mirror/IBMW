#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Cross-build selfdev shm + shm_ext tests for OHOS arm64 and run them under
# qemu-aarch64 and/or on a device through hdc. The working tree is copied to a
# scratch directory because the IBMW core needs ohos/ibmw-core-workaround.diff to
# configure for OHOS; the repository itself is not patched.
#
# Usage: run_shm_ohos.sh [stage ...]    (default: build qemu)
#   build    configure and build the static test binaries and the kernel probe
#   qemu     run both test binaries under qemu-aarch64, once and REPEAT times shuffled
#   device   push to the hdc device; run the kernel probe and both test binaries
#   bench    run ibmw_shm_bench on the device (prints a report, no pass/fail)
#   clean    remove DEVICE_DIR and DEVICE_TMP from the device
#
# Environment:
#   OUT                 outputs (default: <repo>/build-ohos-shm)
#   OHOS_SDK_NATIVE_ROOT SDK native dir (default: /home/oee/xiaoyi/ohos-sdk/18/native)
#   DEPS_DIR            FetchContent sources (default: <repo>/../deps)
#   DEVICE_DIR          binaries on the device (default: /data/local/tmp/ibmw_shm)
#   DEVICE_TMP          test root; keep it short, the service directory is limited
#                       by sun_path (default: /data/local/tmp/s)
#   REPEAT              repeat count (default: 5)
#   JOBS                build parallelism (default: nproc)
#   TEST_TIMEOUT        seconds per test run (default: 1800)
#
# The binaries are fully static because the SDK libc.so is a link stub. Static
# binaries do not exercise the device's dynamic linker namespaces.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../.." && pwd)"
OUT="${OUT:-${REPO}/build-ohos-shm}"
SDK="${OHOS_SDK_NATIVE_ROOT:-/home/oee/xiaoyi/ohos-sdk/18/native}"
DEPS_DIR="${DEPS_DIR:-$(dirname "${REPO}")/deps}"
DEVICE_DIR="${DEVICE_DIR:-/data/local/tmp/ibmw_shm}"
DEVICE_TMP="${DEVICE_TMP:-/data/local/tmp/s}"
REPEAT="${REPEAT:-5}"
JOBS="${JOBS:-$(nproc)}"
TEST_TIMEOUT="${TEST_TIMEOUT:-1800}"
SRC="${OUT}/src"
LOGS="${OUT}/logs"
CORE_TEST="${OUT}/core/shm/ibmw_selfdev_shm_test"
CORE_PEER="${OUT}/core/shm/ibmw_selfdev_shm_peer"
CORE_BENCH="${OUT}/core/shm/ibmw_shm_bench"
EXT_TEST="${OUT}/ext/ibmw_extensions_shm_ext_test"
EXT_PEER="${OUT}/ext/ibmw_shm_ext_peer"
PROBE="${OUT}/shm_probe"
mkdir -p "${LOGS}"

rc=0
fail() {
  echo "FAIL: $*" >&2
  rc=1
}

stage_build() {
  mkdir -p "${SRC}"
  rsync -a --delete --exclude .git --exclude '.omc' --exclude '/build*/' "${REPO}/" "${SRC}/" || return 1
  (cd "${SRC}" && patch -p1 --quiet <"${HERE}/ohos/ibmw-core-workaround.diff") || return 1

  "${SDK}/llvm/bin/clang" --target=aarch64-linux-ohos --sysroot="${SDK}/sysroot" -static -O1 -w \
    "${HERE}/ohos/shm_probe.c" -o "${PROBE}" || return 1

  local common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="${SRC}/cmake/toolchains/ohos-arm64-v8a.cmake"
    -DOHOS_SDK_NATIVE_ROOT="${SDK}" -DOHOS_STL=c++_static -DCMAKE_EXE_LINKER_FLAGS=-static -DIBMW_BUILD_TESTS=ON)
  # Core library: -Wall -Wextra -Wshadow -Werror from ibmw_selfdev_warnings.
  cmake -S "${SRC}/selfdev" -B "${OUT}/core" "${common[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DIBMW_SELFDEV_SHM=ON -DIBMW_SELFDEV_SHM_BENCH=ON -Dgoogletest_LOCAL_SOURCE="${DEPS_DIR}/googletest-src" \
    >"${LOGS}/core.configure.log" 2>&1 || { tail -20 "${LOGS}/core.configure.log"; return 1; }
  cmake --build "${OUT}/core" -j "${JOBS}" >"${LOGS}/core.build.log" 2>&1 || { tail -40 "${LOGS}/core.build.log"; return 1; }

  # Minimal IBMW: runtime + shm_ext only.
  local off=(ICEORYX DDS ECHO GRPC MQTT NET ZENOH PROXY PARAMETER LOG_CONTROL OPENTELEMETRY RECORD_PLAYBACK
    SERVICE_INTROSPECTION TIME_MANIPULATOR TOPIC_LOGGER)
  local args=(-DIBMW_BUILD_SHM_EXTENSION=ON -DIBMW_BUILD_OHOS_CAPTURE_AGENT=OFF -DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=OFF
    -DIBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE=OFF -DIBMW_BUILD_CLI_TOOLS=OFF -DIBMW_BUILD_EXAMPLES=OFF
    -DIBMW_BUILD_PROTOCOLS=OFF -DIBMW_BUILD_ROS2_TYPES=OFF)
  local e d
  for e in "${off[@]}"; do args+=("-DIBMW_BUILD_${e}_EXTENSION=OFF"); done
  for d in ASIO BACKWARD CPPTOML FMT GFLAGS GOOGLETEST ICEORYX JSONCPP LIBUNIFEX NLOHMANN_JSON TBB YAML-CPP; do
    local lower
    lower="$(echo "${d}" | tr '[:upper:]' '[:lower:]')"
    args+=("-DFETCHCONTENT_SOURCE_DIR_${d}=${DEPS_DIR}/${lower}-src")
  done
  cmake -S "${SRC}" -B "${OUT}/ext" "${common[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIBMW_BUILD_WITH_WERROR=ON \
    -DCMAKE_CXX_COMPILER_LAUNCHER="${HERE}/ohos/ibmw-core-launcher.sh" "${args[@]}" \
    >"${LOGS}/ext.configure.log" 2>&1 || { tail -20 "${LOGS}/ext.configure.log"; return 1; }
  cmake --build "${OUT}/ext" -j "${JOBS}" --target ibmw_extensions_shm_ext_test ibmw_shm_ext_peer \
    >"${LOGS}/ext.build.log" 2>&1 || { tail -40 "${LOGS}/ext.build.log"; return 1; }
  echo "build OK"
}

# Prints the gtest summary of a log and returns non-zero unless it has a clean pass.
summarize() {
  local name=$1 run_rc=$2 log=$3
  printf '%-16s rc=%s ' "${name}" "${run_rc}"
  grep -E '^\[  (PASSED|FAILED)  \] [0-9]+ test' "${log}" | sort | uniq -c | tr -s ' ' | tr '\n' ';'
  echo
  [[ "${run_rc}" -eq 0 ]]
}

stage_qemu() {
  local tmp="${OUT}/q"
  rm -rf "${tmp}" && mkdir -p "${tmp}"
  run_qemu() {
    local name=$1 bin=$2 peer_var=$3 peer=$4
    shift 4
    local log="${LOGS}/qemu-${name}.log"
    env TMPDIR="${tmp}" "${peer_var}=${peer}" timeout "${TEST_TIMEOUT}" qemu-aarch64 "${bin}" "$@" >"${log}" 2>&1
    summarize "qemu-${name}" $? "${log}" || fail "qemu-${name} (${log})"
  }
  run_qemu core-once "${CORE_TEST}" IBMW_SHM_PEER_PATH "${CORE_PEER}"
  run_qemu core-repeat "${CORE_TEST}" IBMW_SHM_PEER_PATH "${CORE_PEER}" --gtest_repeat="${REPEAT}" --gtest_shuffle
  run_qemu ext-once "${EXT_TEST}" IBMW_SHM_EXT_PEER_PATH "${EXT_PEER}"
  run_qemu ext-repeat "${EXT_TEST}" IBMW_SHM_EXT_PEER_PATH "${EXT_PEER}" --gtest_repeat="${REPEAT}" --gtest_shuffle
}

stage_device() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  hdc shell "mkdir -p ${DEVICE_DIR}/probe ${DEVICE_TMP}" >/dev/null
  local f
  for f in "${PROBE}" "${CORE_TEST}" "${CORE_PEER}" "${CORE_BENCH}" "${EXT_TEST}" "${EXT_PEER}"; do
    hdc file send "${f}" "${DEVICE_DIR}/$(basename "${f}")" >/dev/null
    hdc shell "chmod 755 ${DEVICE_DIR}/$(basename "${f}")" >/dev/null
  done
  hdc shell "uname -a; getenforce; getconf PAGESIZE; id" | tee "${LOGS}/device-info.log"
  hdc shell "cd ${DEVICE_DIR} && timeout 60 ./shm_probe ${DEVICE_DIR}/probe" | tee "${LOGS}/device-probe.log"
  grep -q '^done' "${LOGS}/device-probe.log" || fail "device probe"
  grep -qE ' FAIL |NOT-REFUSED' "${LOGS}/device-probe.log" && fail "device probe reports a missing feature"

  run_device() {
    local name=$1 bin=$2 peer_var=$3 peer=$4
    shift 4
    local log="${LOGS}/device-${name}.log"
    # hdc does not reliably return the remote exit code: report it in-band.
    hdc shell "cd ${DEVICE_DIR} && TMPDIR=${DEVICE_TMP} ${peer_var}=${DEVICE_DIR}/${peer} timeout ${TEST_TIMEOUT} ./${bin} $* 2>&1; echo DEVICE_RC=\$?" \
      | tr -d '\r' >"${log}"
    local run_rc
    run_rc="$(sed -n 's/^DEVICE_RC=//p' "${log}" | tail -1)"
    summarize "device-${name}" "${run_rc:-255}" "${log}" || fail "device-${name} (${log})"
  }
  run_device core-once ibmw_selfdev_shm_test IBMW_SHM_PEER_PATH ibmw_selfdev_shm_peer
  # RG-39 (group mode between real uids) needs root, which the device shell
  # is; skipped there it proves nothing, so a skip fails this stage.
  run_device core-crossuid ibmw_selfdev_shm_test IBMW_SHM_PEER_PATH ibmw_selfdev_shm_peer "--gtest_filter='*CrossUid*'"
  if ! grep -q '^\[  PASSED  \] 1 test' "${LOGS}/device-core-crossuid.log" ||
    grep -q 'SKIPPED' "${LOGS}/device-core-crossuid.log"; then
    fail "device-core-crossuid did not run as root (${LOGS}/device-core-crossuid.log)"
  fi
  run_device core-repeat ibmw_selfdev_shm_test IBMW_SHM_PEER_PATH ibmw_selfdev_shm_peer --gtest_repeat="${REPEAT}" --gtest_shuffle
  run_device ext-once ibmw_extensions_shm_ext_test IBMW_SHM_EXT_PEER_PATH ibmw_shm_ext_peer
  run_device ext-repeat ibmw_extensions_shm_ext_test IBMW_SHM_EXT_PEER_PATH ibmw_shm_ext_peer --gtest_repeat="${REPEAT}" --gtest_shuffle
}

stage_bench() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  hdc shell "mkdir -p ${DEVICE_DIR} ${DEVICE_TMP}" >/dev/null
  hdc file send "${CORE_BENCH}" "${DEVICE_DIR}/$(basename "${CORE_BENCH}")" >/dev/null
  hdc shell "chmod 755 ${DEVICE_DIR}/$(basename "${CORE_BENCH}")" >/dev/null
  hdc shell "cd ${DEVICE_DIR} && TMPDIR=${DEVICE_TMP} IBMW_SHM_ROOT=${DEVICE_TMP} timeout 600 ./ibmw_shm_bench 2>&1" \
    | tr -d '\r' | tee "${LOGS}/device-bench.log"
}

stage_clean() {
  hdc shell "rm -rf ${DEVICE_DIR} ${DEVICE_TMP}"
}

[[ $# -eq 0 ]] && set -- build qemu
for s in "$@"; do
  echo "==== [shm-ohos] ${s} ===="
  case "${s}" in
    build | qemu | device | bench | clean) "stage_${s}" || fail "stage ${s}" ;;
    *) echo "unknown stage ${s}" >&2; exit 64 ;;
  esac
done
exit "${rc}"
