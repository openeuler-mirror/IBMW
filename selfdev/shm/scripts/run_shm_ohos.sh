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
#   bench-ext  build ibmw_shm_ext_bench from this tree and, with BENCH_BASE_SRC, a
#            baseline tree; run them interleaved on the device, print a summary and
#            remove BENCH_DEVICE_DIR and BENCH_DEVICE_ROOT again (no pass/fail on
#            the numbers). Needs neither `build` nor `device` first.
#   dynamic  dyn-build dyn-device dyn-bench dyn-clean (the plugin as a shared object)
#   dyn-build  build into OUT/dyn: libibmw_shm_transport.so (the real plugin),
#            ibmw_shm_ext_dyn_runner and ibmw_shm_ext_bench_so (rt/), the EXT tests
#            and peer as PIE against libc++_shared.so (test/) and the static
#            ibmw_shm_ext_bench (static/); checks NEEDED/INTERP and writes the TLS
#            model of the plugin and of the static bench to logs/dyn-tls.txt
#   dyn-device push to DYN_DEVICE_DIR with the SDK libc++_shared.so; run the runner as
#            publisher + subscriber process (both dlopen the plugin through the core
#            ExtensionLoader, two load/unload cycles each), then the EXT tests once
#            and REPEAT times shuffled
#   dyn-bench  run three benches interleaved DYN_BENCH_ROUNDS times: static (static
#            musl + c++_static), pie (plugin sources compiled in, device libc +
#            libc++_shared.so) and so (linked against the plugin .so). pie vs static
#            is the libc/libc++ effect, so vs pie the shared-object effect. Results go
#            to a new logs/dyn-bench-<time>/ each run; loadavg and leftover ibmw
#            processes are logged before every round (no pass/fail on the numbers)
#   dyn-clean  remove DYN_DEVICE_DIR and DYN_DEVICE_ROOT from the device
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
#   BENCH_BASE_SRC      baseline tree for bench-ext, e.g. a worktree of 89b5a1a (default: none)
#   BENCH_ROUNDS        bench-ext rounds per binary (default: 25; short rounds, see BENCH_ARGS,
#                       pair better on a device whose speed changes during a run)
#   BENCH_ARGS          extra bench arguments (default: --reps 3 --ms 50)
#   BENCH_CPUS          worker CPUs on the device (default: 12-19, one core type)
#   BENCH_DEVICE_DIR    bench-ext binaries (default: /data/local/tmp/ibmw_shm_bench_ext)
#   BENCH_DEVICE_ROOT   bench-ext IBMW_SHM_ROOT; keep it short (default: /data/local/tmp/ibx)
#   DYN_DEVICE_DIR      dynamic-stage binaries (default: /data/local/tmp/ibmw_shm_dyn)
#   DYN_DEVICE_ROOT     dynamic-stage TMPDIR / IBMW_SHM_ROOT parent; keep it short
#                       (default: /data/local/tmp/ibd)
#   DYN_BENCH_ROUNDS    dyn-bench rounds per binary (default: 15)
#
# build/qemu/device/bench/bench-ext use fully static binaries (-static,
# c++_static), which do not exercise the device's dynamic linker. The SDK
# libc.so is only a link stub, but a PIE linked against it runs with the
# device's /lib/ld-musl-aarch64.so.1; the dyn-* stages use that: the core and
# selfdev archives (STATIC targets, built -fPIC) are linked into the plugin
# .so and into each executable as in production, and every binary shares one
# libc++_shared.so copied from the SDK (OHOS_STL=c++_shared), because the
# runner and the plugin pass std types, YAML nodes and exceptions to each other.
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
BENCH_BASE_SRC="${BENCH_BASE_SRC:-}"
BENCH_ROUNDS="${BENCH_ROUNDS:-25}"
BENCH_ARGS="${BENCH_ARGS:---reps 3 --ms 50}"
BENCH_CPUS="${BENCH_CPUS:-12,13,14,15,16,17,18,19}"
BENCH_DEVICE_DIR="${BENCH_DEVICE_DIR:-/data/local/tmp/ibmw_shm_bench_ext}"
BENCH_DEVICE_ROOT="${BENCH_DEVICE_ROOT:-/data/local/tmp/ibx}"
DYN_DEVICE_DIR="${DYN_DEVICE_DIR:-/data/local/tmp/ibmw_shm_dyn}"
DYN_DEVICE_ROOT="${DYN_DEVICE_ROOT:-/data/local/tmp/ibd}"
DYN_BENCH_ROUNDS="${DYN_BENCH_ROUNDS:-15}"
SRC="${OUT}/src"
LOGS="${OUT}/logs"
CORE_TEST="${OUT}/core/shm/ibmw_selfdev_shm_test"
CORE_PEER="${OUT}/core/shm/ibmw_selfdev_shm_peer"
CORE_BENCH="${OUT}/core/shm/ibmw_shm_bench"
EXT_TEST="${OUT}/ext/ibmw_extensions_shm_ext_test"
EXT_PEER="${OUT}/ext/ibmw_shm_ext_peer"
PROBE="${OUT}/shm_probe"
DYN="${OUT}/dyn"
LIBCXX_SHARED="${SDK}/llvm/lib/aarch64-linux-ohos/libc++_shared.so"
PLUGIN_SO="${DYN}/rt/libibmw_shm_transport.so"
mkdir -p "${LOGS}"

rc=0
fail() {
  echo "FAIL: $*" >&2
  rc=1
}

# common: toolchain and static linking for a copied tree <src>.
common_cmake_args() {
  common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$1/cmake/toolchains/ohos-arm64-v8a.cmake"
    -DOHOS_SDK_NATIVE_ROOT="${SDK}" -DOHOS_STL=c++_static -DCMAKE_EXE_LINKER_FLAGS=-static -DIBMW_BUILD_TESTS=ON)
}

# ext_args: minimal IBMW, runtime + shm_ext only.
ext_cmake_args() {
  local off=(ICEORYX DDS ECHO GRPC MQTT NET ZENOH PROXY PARAMETER LOG_CONTROL OPENTELEMETRY RECORD_PLAYBACK
    SERVICE_INTROSPECTION TIME_MANIPULATOR TOPIC_LOGGER)
  ext_args=(-DIBMW_BUILD_SHM_EXTENSION=ON -DIBMW_BUILD_OHOS_CAPTURE_AGENT=OFF -DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=OFF
    -DIBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE=OFF -DIBMW_BUILD_CLI_TOOLS=OFF -DIBMW_BUILD_EXAMPLES=OFF
    -DIBMW_BUILD_PROTOCOLS=OFF -DIBMW_BUILD_ROS2_TYPES=OFF)
  local e d
  for e in "${off[@]}"; do ext_args+=("-DIBMW_BUILD_${e}_EXTENSION=OFF"); done
  for d in ASIO BACKWARD CPPTOML FMT GFLAGS GOOGLETEST ICEORYX JSONCPP LIBUNIFEX NLOHMANN_JSON TBB YAML-CPP; do
    local lower
    lower="$(echo "${d}" | tr '[:upper:]' '[:lower:]')"
    ext_args+=("-DFETCHCONTENT_SOURCE_DIR_${d}=${DEPS_DIR}/${lower}-src")
  done
  ext_args+=(-DCMAKE_CXX_COMPILER_LAUNCHER="${HERE}/ohos/ibmw-core-launcher.sh")
}

stage_build() {
  mkdir -p "${SRC}"
  rsync -a --delete --exclude .git --exclude '.omc' --exclude '/build*/' "${REPO}/" "${SRC}/" || return 1
  (cd "${SRC}" && patch -p1 --quiet <"${HERE}/ohos/ibmw-core-workaround.diff") || return 1

  "${SDK}/llvm/bin/clang" --target=aarch64-linux-ohos --sysroot="${SDK}/sysroot" -static -O1 -w \
    "${HERE}/ohos/shm_probe.c" -o "${PROBE}" || return 1

  common_cmake_args "${SRC}"
  # Core library: -Wall -Wextra -Wshadow -Werror from ibmw_selfdev_warnings.
  cmake -S "${SRC}/selfdev" -B "${OUT}/core" "${common[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DIBMW_SELFDEV_SHM=ON -DIBMW_SELFDEV_SHM_BENCH=ON -Dgoogletest_LOCAL_SOURCE="${DEPS_DIR}/googletest-src" \
    >"${LOGS}/core.configure.log" 2>&1 || { tail -20 "${LOGS}/core.configure.log"; return 1; }
  cmake --build "${OUT}/core" -j "${JOBS}" >"${LOGS}/core.build.log" 2>&1 || { tail -40 "${LOGS}/core.build.log"; return 1; }

  ext_cmake_args
  cmake -S "${SRC}" -B "${OUT}/ext" "${common[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIBMW_BUILD_WITH_WERROR=ON \
    "${ext_args[@]}" \
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

# bench_ext_build <label> <source tree>: static ibmw_shm_ext_bench in OUT/bench-ext/<label>.
bench_ext_build() {
  local label=$1 from=$2 src="${OUT}/bench-ext/$1/src" dir="${OUT}/bench-ext/$1/build"
  mkdir -p "${src}"
  rsync -a --delete --exclude .git --exclude .omc --exclude '/build*/' "${from}/" "${src}/" || return 1
  "${HERE}/shm_ext_bench_overlay.sh" "${REPO}" "${src}" || return 1
  (cd "${src}" && patch -p1 --quiet <"${HERE}/ohos/ibmw-core-workaround.diff") || return 1
  common_cmake_args "${src}"
  ext_cmake_args
  # IBMW_BUILD_TESTS would add --coverage, whose counters would be measured.
  cmake -S "${src}" -B "${dir}" "${common[@]}" -DIBMW_BUILD_TESTS=OFF -DIBMW_BUILD_SHM_EXT_BENCH=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIBMW_BUILD_WITH_WERROR=ON "${ext_args[@]}" \
    >"${LOGS}/bench-ext-${label}.configure.log" 2>&1 || { tail -20 "${LOGS}/bench-ext-${label}.configure.log"; return 1; }
  cmake --build "${dir}" -j "${JOBS}" --target ibmw_shm_ext_bench \
    >"${LOGS}/bench-ext-${label}.build.log" 2>&1 || { tail -40 "${LOGS}/bench-ext-${label}.build.log"; return 1; }
  find "${dir}" -name ibmw_shm_ext_bench -type f -executable | head -1 | xargs -I{} cp {} "${OUT}/bench-ext/${label}/ibmw_shm_ext_bench"
}

stage_bench-ext() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  device_path_ok "${BENCH_DEVICE_DIR}" "${BENCH_DEVICE_ROOT}" || return 1  # rm -rf ROOT/* below
  local labels=(new) label round log run_rc
  bench_ext_build new "${REPO}" || return 1
  if [[ -n "${BENCH_BASE_SRC}" ]]; then
    bench_ext_build base "${BENCH_BASE_SRC}" || return 1
    labels=(base new)
  fi
  hdc shell "mkdir -p ${BENCH_DEVICE_DIR} ${BENCH_DEVICE_ROOT}" >/dev/null
  for label in "${labels[@]}"; do
    hdc file send "${OUT}/bench-ext/${label}/ibmw_shm_ext_bench" "${BENCH_DEVICE_DIR}/bench_${label}" >/dev/null
    hdc shell "chmod 755 ${BENCH_DEVICE_DIR}/bench_${label}" >/dev/null
  done
  local runs="${LOGS}/bench-ext-runs"
  rm -rf "${runs}" && mkdir -p "${runs}"
  for ((round = 1; round <= BENCH_ROUNDS; ++round)); do
    for label in "${labels[@]}"; do
      log="${runs}/${label}.${round}.log"
      echo "round ${round} ${label} load: $(hdc shell 'cat /proc/loadavg' | tr -d '\r' | cut -d' ' -f1)"
      # hdc does not reliably return the remote exit code: report it in-band.
      hdc shell "cd ${BENCH_DEVICE_DIR} && IBMW_SHM_ROOT=${BENCH_DEVICE_ROOT} timeout 300 ./bench_${label} --cpus ${BENCH_CPUS} ${BENCH_ARGS} 2>&1; echo DEVICE_RC=\$?; rm -rf ${BENCH_DEVICE_ROOT}/*" \
        | tr -d '\r' >"${log}"
      run_rc="$(sed -n 's/^DEVICE_RC=//p' "${log}" | tail -1)"
      [[ "${run_rc:-255}" -eq 0 ]] || fail "bench-ext ${label} round ${round} (${log})"
    done
  done
  device_rm "${BENCH_DEVICE_DIR}" "${BENCH_DEVICE_ROOT}" >/dev/null
  python3 "${HERE}/shm_ext_bench_summary.py" "${runs}" | tee "${LOGS}/bench-ext-summary.txt"
}

# device_path_ok <path...>: true when every path is strictly below
# /data/local/tmp and made only of [A-Za-z0-9._-] components (no "..", spaces,
# globs or shell metacharacters). The directory variables can be overridden and
# are expanded by the device shell, so they are checked before any rm.
device_path_ok() {
  local p
  for p in "$@"; do
    # Each component needs a character other than "." (rejects "." and "..").
    [[ "${p}" =~ ^/data/local/tmp(/[A-Za-z0-9._-]*[A-Za-z0-9_-][A-Za-z0-9._-]*)+$ && "${p}" != *..* ]] ||
      { echo "refusing to use ${p} on the device" >&2; return 1; }
  done
}

# device_rm <path...>: rm -rf on the device for paths that pass device_path_ok.
device_rm() {
  device_path_ok "$@" || return 1
  hdc shell "rm -rf $*"
}

stage_clean() {
  device_rm "${DEVICE_DIR}" "${DEVICE_TMP}"
}

# Our processes on the device: the dynamic-stage binaries by name.
DYN_PROC_NAMES=(ibmw_shm_ext_dyn_runner ibmw_extensions_shm_ext_test ibmw_shm_ext_peer bench_static bench_pie bench_so)
DYN_PROCS="$(IFS='|'; echo "${DYN_PROC_NAMES[*]}")"

# On Ctrl-C or SIGTERM during a dyn stage, stop what still runs on the device
# (hdc does not forward the signal) and remove the dynamic-stage directories.
dyn_interrupted() {
  trap - INT TERM
  echo "interrupted: stopping device processes and removing ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT}" >&2
  local name
  # Started as ./<name> or, for the EXT peer, by absolute path. The pattern
  # [/]<name> matches both but not the command line of the device shell running
  # pkill (which contains "]<name>").
  for name in "${DYN_PROC_NAMES[@]}"; do hdc shell "pkill -f '[/]${name}'" >/dev/null 2>&1; done
  device_rm "${DYN_DEVICE_DIR}" "${DYN_DEVICE_ROOT}" >/dev/null 2>&1
  exit 130
}

# dyn_cmake <name> <stl> <extra cmake args...>: configure OUT/dyn/<name> from OUT/dyn/src.
dyn_cmake() {
  local name=$1 stl=$2
  shift 2
  ext_cmake_args
  cmake -S "${DYN}/src" -B "${DYN}/${name}" -G Ninja -DCMAKE_TOOLCHAIN_FILE="${DYN}/src/cmake/toolchains/ohos-arm64-v8a.cmake" \
    -DOHOS_SDK_NATIVE_ROOT="${SDK}" -DOHOS_STL="${stl}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIBMW_BUILD_WITH_WERROR=ON \
    "${ext_args[@]}" "$@" >"${LOGS}/dyn-${name}.configure.log" 2>&1 || { tail -20 "${LOGS}/dyn-${name}.configure.log"; return 1; }
}

dyn_make() {
  local name=$1
  shift
  cmake --build "${DYN}/${name}" -j "${JOBS}" --target "$@" >"${LOGS}/dyn-${name}.build.log" 2>&1 ||
    { tail -40 "${LOGS}/dyn-${name}.build.log"; return 1; }
}

# TLS access in the plugin .so and in the static bench: which runtime or
# relocation each one uses, and the code of ShmOperationGate::TryAcquire, the
# hot path that reads the ThreadStripe() thread_local.
tls_report() {
  local f bin=("${SDK}/llvm/bin/llvm-objdump" -d --no-show-raw-insn)
  local readelf="${SDK}/llvm/bin/llvm-readelf" nm="${SDK}/llvm/bin/llvm-nm"
  for f in "$@"; do
    echo "== $(basename "${f}")"
    echo "PT_TLS segments: $("${readelf}" -lW "${f}" | grep -c ' TLS ')"
    echo "bl __emutls_get_address: $("${bin[@]}" "${f}" | grep -c 'bl.*<__emutls_get_address')"
    echo "__emutls_get_address symbol: $("${readelf}" -sW "${f}" | awk '$NF == "__emutls_get_address" {v = $5 " " $6 " " $7} END {print v}')"
    echo "__emutls_v.* control objects: $("${nm}" "${f}" | grep -c ' __emutls_v\.')"
    echo "bl __tls_get_addr: $("${bin[@]}" "${f}" | grep -c 'bl.*<__tls_get_addr')"
    echo "__tls_get_addr relocations: $("${readelf}" -rW "${f}" | grep -c '__tls_get_addr')"
    echo "TLSDESC relocations: $("${readelf}" -rW "${f}" | grep -c 'TLSDESC')"
    echo "TLS_TPREL (initial-exec) relocations: $("${readelf}" -rW "${f}" | grep -c 'TLS_TPREL')"
    echo "mrs tpidr_el0 (direct thread-pointer reads): $("${bin[@]}" "${f}" | grep -c 'mrs.*tpidr_el0')"
    echo "shm_ext thread_locals:"
    "${nm}" -C "${f}" | grep -E '__emutls_v\..*(t_stripe|t_current_subscription|t_listener_thread_ready|TopicKey)' | sed 's/^/  /'
    echo "ShmOperationGate::TryAcquire:"
    "${bin[@]}" --disassemble-symbols=_ZN4ibmw10extensions13shm_extension16ShmOperationGate10TryAcquireEv "${f}" |
      grep -E '^\s+[0-9a-f]+:' | sed -n '1,24s/^/  /p'
  done
}

stage_dyn-build() {
  mkdir -p "${DYN}/src"
  rsync -a --delete --exclude .git --exclude '.omc' --exclude '/build*/' "${REPO}/" "${DYN}/src/" || return 1
  (cd "${DYN}/src" && patch -p1 --quiet <"${HERE}/ohos/ibmw-core-workaround.diff") || return 1
  [[ -f "${LIBCXX_SHARED}" ]] || { echo "missing ${LIBCXX_SHARED}"; return 1; }
  # rt: no IBMW_BUILD_TESTS, whose --coverage would be measured by the bench and
  # would make the plugin write counters when it is unloaded.
  dyn_cmake rt c++_shared -DIBMW_BUILD_TESTS=OFF -DIBMW_BUILD_SHM_EXT_BENCH=ON -DIBMW_BUILD_SHM_EXT_DYN_CHECK=ON || return 1
  dyn_make rt ibmw_extensions_shm_ext ibmw_shm_ext_dyn_runner ibmw_shm_ext_bench_so ibmw_shm_ext_bench || return 1
  # test: the EXT tests compile the plugin sources in (they need its hidden
  # symbols and the one copy of the selfdev state), linked dynamically.
  dyn_cmake test c++_shared -DIBMW_BUILD_TESTS=ON || return 1
  dyn_make test ibmw_extensions_shm_ext_test ibmw_shm_ext_peer || return 1
  # static: the bench as bench-ext builds it, the base of dyn-bench.
  dyn_cmake static c++_static -DCMAKE_EXE_LINKER_FLAGS=-static -DIBMW_BUILD_TESTS=OFF -DIBMW_BUILD_SHM_EXT_BENCH=ON || return 1
  dyn_make static ibmw_shm_ext_bench || return 1

  local readelf="${SDK}/llvm/bin/llvm-readelf" f
  for f in "${DYN}/rt/ibmw_shm_ext_dyn_runner" "${DYN}/rt/ibmw_shm_ext_bench_so" "${DYN}/rt/ibmw_shm_ext_bench" \
    "${DYN}/test/ibmw_extensions_shm_ext_test" \
    "${DYN}/test/ibmw_shm_ext_peer"; do
    "${readelf}" -lW "${f}" | grep 'ld-musl-aarch64.so.1' >/dev/null || { echo "${f}: no dynamic interpreter"; return 1; }
    "${readelf}" -dW "${f}" | grep 'NEEDED.*libc++_shared.so' >/dev/null || { echo "${f}: not linked to libc++_shared.so"; return 1; }
  done
  "${readelf}" -dW "${PLUGIN_SO}" | grep 'NEEDED.*libc++_shared.so' >/dev/null || { echo "plugin not linked to libc++_shared.so"; return 1; }
  # The runner must reach the plugin only through dlopen; the bench_so links it.
  "${readelf}" -dW "${DYN}/rt/ibmw_shm_ext_dyn_runner" | grep 'NEEDED.*libibmw_shm_transport' >/dev/null &&
    { echo "runner links the plugin"; return 1; }
  "${readelf}" -dW "${DYN}/rt/ibmw_shm_ext_bench_so" | grep 'NEEDED.*libibmw_shm_transport.so' >/dev/null ||
    { echo "bench_so does not link the plugin"; return 1; }
  "${readelf}" -dW "${DYN}/rt/ibmw_shm_ext_bench" | grep 'NEEDED.*libibmw_shm_transport' >/dev/null &&
    { echo "pie bench links the plugin"; return 1; }
  "${readelf}" -lW "${DYN}/static/ibmw_shm_ext_bench" | grep INTERP >/dev/null && { echo "static bench is dynamic"; return 1; }
  tls_report "${PLUGIN_SO}" "${DYN}/static/ibmw_shm_ext_bench" >"${LOGS}/dyn-tls.txt"
  echo "dyn-build OK (TLS report: ${LOGS}/dyn-tls.txt)"
}

# dyn_push <file> [name]: copy to DYN_DEVICE_DIR.
dyn_push() {
  local to="${DYN_DEVICE_DIR}/${2:-$(basename "$1")}"
  hdc file send "$1" "${to}" >/dev/null
  hdc shell "chmod 755 ${to}" >/dev/null
}

stage_dyn-device() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  trap dyn_interrupted INT TERM
  hdc shell "mkdir -p ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT}" >/dev/null
  local f
  for f in "${LIBCXX_SHARED}" "${PLUGIN_SO}" "${DYN}/rt/ibmw_shm_ext_dyn_runner" "${DYN}/test/ibmw_extensions_shm_ext_test" \
    "${DYN}/test/ibmw_shm_ext_peer"; do
    dyn_push "${f}"
  done
  local env="LD_LIBRARY_PATH=${DYN_DEVICE_DIR} TMPDIR=${DYN_DEVICE_ROOT}"
  local log="${LOGS}/dyn-runner.log" root="${DYN_DEVICE_ROOT}/r"
  local args="--plugin ${DYN_DEVICE_DIR}/libibmw_shm_transport.so --sync ${root}/sync --topics 20 --early 6 --messages 2000 --cycles 2"
  # One shell runs both processes; hdc does not reliably return the remote exit
  # code, so both are reported in-band.
  device_rm "${root}" >/dev/null || return 1
  hdc shell "mkdir -p ${root}/s ${root}/sync && cd ${DYN_DEVICE_DIR} && export ${env} IBMW_SHM_ROOT=${root}/s; \
    (timeout 300 ./ibmw_shm_ext_dyn_runner sub ${args} >${root}/sub.log 2>&1; echo SUB_RC=\$? >>${root}/sub.log) & \
    timeout 300 ./ibmw_shm_ext_dyn_runner pub ${args} >${root}/pub.log 2>&1; echo PUB_RC=\$? >>${root}/pub.log; wait; \
    cat ${root}/pub.log ${root}/sub.log" | tr -d '\r' >"${log}"
  grep -E '^(DYN_|PUB_RC|SUB_RC)' "${log}" | grep -v '^DYN_ABI .*result=PASS$'
  grep '^DYN_ABI' "${log}" | sort -u -t' ' -k2,2 | sed 's/^/  /' 
  if ! grep -q '^PUB_RC=0$' "${log}" || ! grep -q '^SUB_RC=0$' "${log}" ||
    [[ "$(grep -c '^DYN_CYCLE .*result=PASS$' "${log}")" -ne 4 ]] ||
    [[ "$(grep -c '^DYN_THROW .*result=PASS$' "${log}")" -ne 2 ]] || grep -q '^DYN_ABI .*result=FAIL$' "${log}" ||
    ! grep -q '^DYN_RESULT role=pub cycles=2 result=PASS' "${log}" || ! grep -q '^DYN_RESULT role=sub cycles=2 result=PASS' "${log}"; then
    fail "dyn-runner (${log})"
  fi

  run_dyn_test() {
    local name=$1
    shift
    log="${LOGS}/dyn-${name}.log"
    hdc shell "cd ${DYN_DEVICE_DIR} && ${env} IBMW_SHM_EXT_PEER_PATH=${DYN_DEVICE_DIR}/ibmw_shm_ext_peer timeout ${TEST_TIMEOUT} ./ibmw_extensions_shm_ext_test $* 2>&1; echo DEVICE_RC=\$?" \
      | tr -d '\r' >"${log}"
    local run_rc
    run_rc="$(sed -n 's/^DEVICE_RC=//p' "${log}" | tail -1)"
    summarize "dyn-${name}" "${run_rc:-255}" "${log}" || fail "dyn-${name} (${log})"
  }
  run_dyn_test ext-once
  run_dyn_test ext-repeat --gtest_repeat="${REPEAT}" --gtest_shuffle
}

stage_dyn-bench() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  device_path_ok "${DYN_DEVICE_DIR}" "${DYN_DEVICE_ROOT}" || return 1  # rm -rf ROOT/b/* below
  trap dyn_interrupted INT TERM
  hdc shell "mkdir -p ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT}/b" >/dev/null
  dyn_push "${LIBCXX_SHARED}"
  dyn_push "${PLUGIN_SO}"
  dyn_push "${DYN}/static/ibmw_shm_ext_bench" bench_static
  dyn_push "${DYN}/rt/ibmw_shm_ext_bench" bench_pie
  dyn_push "${DYN}/rt/ibmw_shm_ext_bench_so" bench_so
  # A new directory per run: earlier results stay where the docs cite them.
  local runs="${LOGS}/dyn-bench-$(date +%Y%m%d-%H%M%S)" round label log run_rc others
  mkdir -p "${runs}"
  for ((round = 1; round <= DYN_BENCH_ROUNDS; ++round)); do
    for label in static pie so; do
      log="${runs}/${label}.${round}.log"
      others="$(hdc shell "ps -ef | grep -E '${DYN_PROCS}' | grep -v grep" | tr -d '\r')"
      [[ -z "${others}" ]] || echo "WARN: round ${round} ${label}: our processes still on the device: ${others}"
      echo "round ${round} ${label} loadavg $(hdc shell 'cat /proc/loadavg' | tr -d '\r')" | tee -a "${runs}/load.txt"
      hdc shell "cd ${DYN_DEVICE_DIR} && LD_LIBRARY_PATH=${DYN_DEVICE_DIR} IBMW_SHM_ROOT=${DYN_DEVICE_ROOT}/b timeout 300 ./bench_${label} --cpus ${BENCH_CPUS} ${BENCH_ARGS} 2>&1; echo DEVICE_RC=\$?; rm -rf ${DYN_DEVICE_ROOT}/b/*" \
        | tr -d '\r' >"${log}"
      run_rc="$(sed -n 's/^DEVICE_RC=//p' "${log}" | tail -1)"
      [[ "${run_rc:-255}" -eq 0 ]] || fail "dyn-bench ${label} round ${round} (${log})"
    done
  done
  {
    echo "# ${runs}: ${DYN_BENCH_ROUNDS} rounds, ${BENCH_ARGS}, CPUs ${BENCH_CPUS}"
    echo "# static = -static (SDK musl, c++_static); pie = plugin sources compiled in, device libc + libc++_shared.so;"
    echo "# so = linked against libibmw_shm_transport.so"
    echo; echo "## pie vs static (libc/libc++)"; python3 "${HERE}/shm_ext_bench_summary.py" "${runs}" static pie
    echo; echo "## so vs pie (shared object)"; python3 "${HERE}/shm_ext_bench_summary.py" "${runs}" pie so
  } | tee "${runs}/summary.txt"
}

stage_dyn-clean() {
  device_rm "${DYN_DEVICE_DIR}" "${DYN_DEVICE_ROOT}"
  local rc=$?
  trap - INT TERM
  return "${rc}"
}

stage_dynamic() {
  stage_dyn-build || return 1
  local ok=0
  stage_dyn-device || ok=1
  stage_dyn-bench || ok=1
  stage_dyn-clean
  return "${ok}"
}

[[ $# -eq 0 ]] && set -- build qemu
for s in "$@"; do
  echo "==== [shm-ohos] ${s} ===="
  case "${s}" in
    build | qemu | device | bench | bench-ext | clean | dynamic | dyn-build | dyn-device | dyn-bench | dyn-clean)
      "stage_${s}" || fail "stage ${s}" ;;
    *) echo "unknown stage ${s}" >&2; exit 64 ;;
  esac
done
exit "${rc}"
