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
#   dynamic  dyn-build dyn-device dyn-uid dyn-bench dyn-clean (the plugin as a shared object)
#   dyn-build  build into OUT/dyn: libibmw_shm_transport.so (the real plugin),
#            ibmw_shm_ext_dyn_runner and ibmw_shm_ext_bench_so (rt/), the EXT tests
#            and peer as PIE against libc++_shared.so (test/) and the static
#            ibmw_shm_ext_bench (static/); checks NEEDED/INTERP and writes the TLS
#            model of the plugin and of the static bench to logs/dyn-tls.txt
#   dyn-device push to DYN_DEVICE_DIR with the SDK libc++_shared.so; run the runner as
#            publisher + subscriber process (both dlopen the plugin through the core
#            ExtensionLoader, two load/unload cycles each), then the EXT tests once
#            and REPEAT times shuffled
#   dyn-uid  the dyn-device runner and EXT tests again as each uid of RUN_UIDS
#            instead of root: ohos/run_as.c takes the uid and gid without
#            supplementary groups and clears the capabilities (HongMeng keeps them
#            across setuid), checks that a root-owned 0700 directory refuses it,
#            then execs the test; the SELinux domain stays su. Needs dyn-build
#   dyn-bench  run three benches interleaved DYN_BENCH_ROUNDS times: static (static
#            musl + c++_static), pie (plugin sources compiled in, device libc +
#            libc++_shared.so) and so (linked against the plugin .so). pie vs static
#            is the libc/libc++ effect, so vs pie the shared-object effect. Results go
#            to a new logs/dyn-bench-<time>/ each run; loadavg and leftover ibmw
#            processes are logged before every round (no pass/fail on the numbers)
#   dyn-clean  remove DYN_DEVICE_DIR and DYN_DEVICE_ROOT from the device
#   hap      dyn-build hap-build hap-device (the tests inside an application process)
#   hap-build  build the test HAP into OUT/hap/unsigned.hap: the plugin, the
#            runner and the EXT tests as shared objects (dyn trees with
#            IBMW_BUILD_SHM_EXT_HAP_CHECK), the NAPI module that starts them as
#            native child processes, the ArkTS test runner (es2abc) and the
#            resources (restool, app_packing_tool). Needs dyn-build (it also
#            copies the sources)
#   hap-device sign the HAP for the connected device (OpenHarmony test
#            certificate; debug profile with the device UDID, or the release
#            profile with HAP_SIGN=release), install HAP_BUNDLE, run it with
#            `aa test`, copy the logs from the application sandbox to
#            logs/hap-<time>/ (hap-release-<time>/), check them and uninstall
#            the bundle
#   hap-release  hap with HAP_SIGN=release (normal_hap domain)
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
#   RUN_UIDS            dyn-uid uids, gid = uid (default: 2000 20099999: the shell
#                       uid and an unused uid of the user-100 application range)
#   HAP_BUNDLE          bundle name of the test HAP (default: com.ibmw.shmhap); it
#                       is installed and uninstalled again by hap-device
#   HAP_SIGN            debug (default): debug profile with the device UDID and
#                       "debug": true, runs in u:r:debug_hap:s0; release: release
#                       profile (os_integration) and "debug": false, runs in
#                       u:r:normal_hap:s0. hap-build and hap-device must agree
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
RUN_UIDS="${RUN_UIDS:-2000 20099999}"
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
RUN_AS="${OUT}/run_as"
HAP="${OUT}/hap"
HAP_BUNDLE="${HAP_BUNDLE:-com.ibmw.shmhap}"
HAP_SIGN="${HAP_SIGN:-debug}"
SDK_ROOT="$(dirname "${SDK}")"
SIGN_LIB="${SDK_ROOT}/toolchains/lib"
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
  # Short: the service directories below it must fit the socket path limit
  # (63 bytes); OUT/q is too deep for most checkouts.
  local tmp
  tmp="$(mktemp -d /tmp/ibq.XXXXXX)" || return 1
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
  rm -rf "${tmp}"
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
DYN_PROC_NAMES=(run_as ibmw_shm_ext_dyn_runner ibmw_extensions_shm_ext_test ibmw_shm_ext_peer bench_static bench_pie bench_so)
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

# dyn_push <file> [name]: copy to DYN_DEVICE_DIR. The old copy is removed first
# and the size compared afterwards (hdc does not reliably report failures), so
# a missing artifact or a failed send cannot leave a stale binary to be tested.
dyn_push() {
  local to="${DYN_DEVICE_DIR}/${2:-$(basename "$1")}" size
  [[ -f "$1" ]] || { fail "missing $1 (run dyn-build)"; return 1; }
  size="$(stat -c %s "$1")"
  hdc shell "rm -f ${to}" >/dev/null
  hdc file send "$1" "${to}" >/dev/null
  hdc shell "chmod 755 ${to}" >/dev/null
  [[ "$(hdc shell "stat -c %s ${to} 2>/dev/null" | tr -d '\r')" == "${size}" ]] || { fail "push ${to}"; return 1; }
}

stage_dyn-device() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  device_path_ok "${DYN_DEVICE_DIR}" "${DYN_DEVICE_ROOT}" || return 1
  trap dyn_interrupted INT TERM
  hdc shell "mkdir -p ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT}" >/dev/null
  local f
  for f in "${LIBCXX_SHARED}" "${PLUGIN_SO}" "${DYN}/rt/ibmw_shm_ext_dyn_runner" "${DYN}/test/ibmw_extensions_shm_ext_test" \
    "${DYN}/test/ibmw_shm_ext_peer"; do
    dyn_push "${f}" || return 1
  done
  device_rm "${DYN_DEVICE_ROOT}/r" >/dev/null || return 1
  hdc shell "mkdir -p ${DYN_DEVICE_ROOT}/r/s ${DYN_DEVICE_ROOT}/r/sync" >/dev/null
  dyn_runner_check dyn-runner "${DYN_DEVICE_ROOT}/r" ""
  dyn_test_check dyn-ext-once "${DYN_DEVICE_ROOT}" ""
  dyn_test_check dyn-ext-repeat "${DYN_DEVICE_ROOT}" "" --gtest_repeat="${REPEAT}" --gtest_shuffle
}

# dyn_runner_check <name> <dir> <prefix>: the runner as publisher + subscriber
# process with TMPDIR=<dir>, IBMW_SHM_ROOT=<dir>/s and the sync directory
# <dir>/sync (both must exist), each started through <prefix> (empty, or
# run_as ...); log LOGS/<name>.log (<name> may contain a directory). The device
# shell writes the process output to DYN_DEVICE_ROOT/<last part of name>.{pub,sub}.log,
# outside <dir>.
dyn_runner_check() {
  local name=$1 root=$2 prefix=$3 log="${LOGS}/$1.log" out="${DYN_DEVICE_ROOT}/${1##*/}"
  local args="--plugin ${DYN_DEVICE_DIR}/libibmw_shm_transport.so --sync ${root}/sync --topics 20 --early 6 --messages 2000 --cycles 2"
  # One shell runs both processes; hdc does not reliably return the remote exit
  # code, so both are reported in-band.
  hdc shell "cd ${DYN_DEVICE_DIR} && export LD_LIBRARY_PATH=${DYN_DEVICE_DIR} TMPDIR=${root} IBMW_SHM_ROOT=${root}/s; \
    (timeout 300 ${prefix} ./ibmw_shm_ext_dyn_runner sub ${args} >${out}.sub.log 2>&1; echo SUB_RC=\$? >>${out}.sub.log) & \
    timeout 300 ${prefix} ./ibmw_shm_ext_dyn_runner pub ${args} >${out}.pub.log 2>&1; echo PUB_RC=\$? >>${out}.pub.log; wait; \
    cat ${out}.pub.log ${out}.sub.log; rm -f ${out}.pub.log ${out}.sub.log" | tr -d '\r' >"${log}"
  grep -E '^(RUN_AS|PROC_CREDS|DYN_|PUB_RC|SUB_RC)' "${log}" | grep -v '^DYN_ABI .*result=PASS$'
  grep '^DYN_ABI' "${log}" | sort -u -t' ' -k2,2 | sed 's/^/  /'
  if ! grep -q '^PUB_RC=0$' "${log}" || ! grep -q '^SUB_RC=0$' "${log}" ||
    [[ "$(grep -c '^DYN_CYCLE .*result=PASS$' "${log}")" -ne 4 ]] ||
    [[ "$(grep -c '^DYN_THROW .*result=PASS$' "${log}")" -ne 2 ]] || grep -q '^DYN_ABI .*result=FAIL$' "${log}" ||
    ! grep -q '^DYN_RESULT role=pub cycles=2 result=PASS' "${log}" || ! grep -q '^DYN_RESULT role=sub cycles=2 result=PASS' "${log}"; then
    fail "${name} (${log})"
  fi
}

# dyn_test_check <name> <tmpdir> <prefix> [gtest args...]: the EXT tests (and the
# peers they spawn) with TMPDIR=<tmpdir>, started through <prefix>.
dyn_test_check() {
  local name=$1 tmp=$2 prefix=$3 log="${LOGS}/$1.log"
  shift 3
  hdc shell "cd ${DYN_DEVICE_DIR} && LD_LIBRARY_PATH=${DYN_DEVICE_DIR} TMPDIR=${tmp} IBMW_SHM_EXT_PEER_PATH=${DYN_DEVICE_DIR}/ibmw_shm_ext_peer timeout ${TEST_TIMEOUT} ${prefix} ./ibmw_extensions_shm_ext_test $* 2>&1; echo DEVICE_RC=\$?" \
    | tr -d '\r' >"${log}"
  local run_rc
  run_rc="$(sed -n 's/^DEVICE_RC=//p' "${log}" | tail -1)"
  summarize "${name}" "${run_rc:-255}" "${log}" || fail "${name} (${log})"
}

# creds_check <log> <uid> <bin>=<min count>...: every PROC_CREDS line of <log>
# (printed by the runner, the EXT tests and the peer about themselves) shows
# <uid>/<uid> for all ids, no supplementary groups and no capabilities, and each
# <bin> printed at least <min count> of them.
creds_check() {
  local log=$1 uid=$2 want bin min z=0000000000000000
  shift 2
  want="uid=${uid},${uid},${uid},${uid} gid=${uid},${uid},${uid},${uid} groups= CapInh=${z} CapPrm=${z} CapEff=${z} CapAmb=${z}"
  grep '^PROC_CREDS ' "${log}" | grep -vxE "PROC_CREDS bin=[a-z_]+ ${want}" >/dev/null &&
    { fail "privileges in ${log}: $(grep '^PROC_CREDS ' "${log}" | grep -vxE "PROC_CREDS bin=[a-z_]+ ${want}" | head -1)"; }
  for want in "$@"; do
    bin=${want%%=*} min=${want#*=}
    [[ "$(grep -c "^PROC_CREDS bin=${bin} " "${log}")" -ge "${min}" ]] || fail "${log}: fewer than ${min} PROC_CREDS lines of ${bin}"
  done
}

stage_dyn-uid() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  device_path_ok "${DYN_DEVICE_DIR}" "${DYN_DEVICE_ROOT}" || return 1
  trap dyn_interrupted INT TERM
  "${SDK}/llvm/bin/clang" --target=aarch64-linux-ohos --sysroot="${SDK}/sysroot" -static -O1 -Wall -Wextra -Werror \
    "${HERE}/ohos/run_as.c" -o "${RUN_AS}" || return 1
  hdc shell "mkdir -p ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT}" >/dev/null
  local f
  for f in "${LIBCXX_SHARED}" "${PLUGIN_SO}" "${DYN}/rt/ibmw_shm_ext_dyn_runner" "${DYN}/test/ibmw_extensions_shm_ext_test" \
    "${DYN}/test/ibmw_shm_ext_peer" "${RUN_AS}"; do
    dyn_push "${f}" || return 1
  done
  # A new directory per run, so the logs of a failed run are kept.
  local runs
  runs="dyn-uid-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "${LOGS}/${runs}"
  local deny="${DYN_DEVICE_ROOT}/deny" uid dir others prefix n=0 ctx left
  # run_as does not change the SELinux domain: the uids keep that of the shell.
  ctx="$(hdc shell "cat /proc/self/attr/current" | tr -d '\r\0')"
  [[ -n "${ctx}" ]] || { fail "dyn-uid: cannot read the SELinux context"; return 1; }
  # The uids reach only DYN_DEVICE_DIR (0755) and their own 0700 directory.
  hdc shell "chmod 755 ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT} && mkdir -p ${deny} && chmod 700 ${deny} && rm -f ${deny}/run_as_probe.*" >/dev/null
  for uid in ${RUN_UIDS}; do
    [[ "${uid}" =~ ^[1-9][0-9]*$ ]] || { fail "dyn-uid: bad uid ${uid}"; continue; }
    # Short: the service directory <dir>/ibmwx.XXXXXX/ibmw_shm.<uid>.default of
    # the EXT tests must stay within 63 bytes, and app uids have 8 digits.
    dir="${DYN_DEVICE_ROOT}/u$((++n))"
    local base="${runs}/uid${uid}"
    prefix="./run_as ${uid} ${uid} ${deny}"
    others="$(hdc shell "ps -A -o UID,PID,ARGS | awk '\$1 == ${uid}'" | tr -d '\r')"
    [[ -z "${others}" ]] || echo "NOTE: uid ${uid} already has processes on the device: ${others}"
    device_rm "${dir}" >/dev/null || return 1
    hdc shell "mkdir -p ${dir}/s ${dir}/sync && chown -R ${uid}:${uid} ${dir} && chmod 700 ${dir}" >/dev/null
    # The launcher about itself after the exec; the test binaries report
    # themselves through PROC_CREDS below.
    hdc shell "cd ${DYN_DEVICE_DIR} && ${prefix} ./run_as --caps 2>&1" | tr -d '\r' | tee "${LOGS}/${base}-caps.log"
    grep -q "^RUN_AS_CAPS uid=${uid} euid=${uid} gid=${uid} egid=${uid} groups=0 CapInh=0\{16\} CapEff=0\{16\} CapPrm=0\{16\} .*CapAmb=0\{16\} context=${ctx}$" \
      "${LOGS}/${base}-caps.log" || fail "dyn-uid${uid}: privileges or domain changed after exec (${LOGS}/${base}-caps.log)"
    dyn_runner_check "${base}-runner" "${dir}" "${prefix}"
    [[ "$(grep -c "^RUN_AS uid=${uid} .*result=PASS context=${ctx}$" "${LOGS}/${base}-runner.log")" -eq 2 ]] ||
      fail "dyn-uid${uid}-runner: not both processes as uid ${uid}"
    creds_check "${LOGS}/${base}-runner.log" "${uid}" runner=2
    dyn_test_check "${base}-ext-once" "${dir}" "${prefix}"
    dyn_test_check "${base}-ext-repeat" "${dir}" "${prefix}" --gtest_repeat="${REPEAT}" --gtest_shuffle
    for f in ext-once ext-repeat; do
      grep -q "^RUN_AS uid=${uid} .*result=PASS context=${ctx}$" "${LOGS}/${base}-${f}.log" || fail "dyn-uid${uid}-${f}: not run as uid ${uid}"
      creds_check "${LOGS}/${base}-${f}.log" "${uid}" ext_test=1 peer=1
    done
    # What is left in the directory must belong to the uid (the EXT tests
    # remove their own tree on exit, so this mostly covers the runner).
    hdc shell "find ${dir} ! -user ${uid} -print 2>&1" | tr -d '\r' >"${LOGS}/${base}-foreign.log"
    [[ ! -s "${LOGS}/${base}-foreign.log" ]] || fail "dyn-uid${uid}: files not owned by ${uid} (${LOGS}/${base}-foreign.log)"
    # Nothing of ours may still run as the uid (e.g. a peer after a timeout).
    left="$(hdc shell "ps -A -o UID,PID,ARGS | awk '\$1 == ${uid}'" | tr -d '\r' | grep -E "${DYN_PROCS}")"
    if [[ -n "${left}" ]]; then
      fail "dyn-uid${uid}: processes left: ${left}"
      for f in "${DYN_PROC_NAMES[@]}"; do hdc shell "pkill -f '[/]${f}'" >/dev/null 2>&1; done
    fi
    device_rm "${dir}" >/dev/null || fail "dyn-uid${uid}: removing ${dir}"
  done
  [[ "${n}" -gt 0 ]] || fail "dyn-uid: no valid uid in RUN_UIDS"
  device_rm "${deny}" >/dev/null || fail "dyn-uid: removing ${deny}"
  echo "dyn-uid logs: ${LOGS}/${runs}"
}

stage_dyn-bench() {
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  device_path_ok "${DYN_DEVICE_DIR}" "${DYN_DEVICE_ROOT}" || return 1  # rm -rf ROOT/b/* below
  trap dyn_interrupted INT TERM
  hdc shell "mkdir -p ${DYN_DEVICE_DIR} ${DYN_DEVICE_ROOT}/b" >/dev/null
  dyn_push "${LIBCXX_SHARED}" || return 1
  dyn_push "${PLUGIN_SO}" || return 1
  dyn_push "${DYN}/static/ibmw_shm_ext_bench" bench_static || return 1
  dyn_push "${DYN}/rt/ibmw_shm_ext_bench" bench_pie || return 1
  dyn_push "${DYN}/rt/ibmw_shm_ext_bench_so" bench_so || return 1
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
  stage_dyn-uid || ok=1
  stage_dyn-bench || ok=1
  stage_dyn-clean
  return "${ok}"
}

# ---------------------------------------------------------------- test HAP

hap_check_libs() {
  local readelf="${SDK}/llvm/bin/llvm-readelf" f
  for f in "$@"; do
    "${readelf}" -dW "${f}" | grep 'NEEDED.*libc++_shared.so' >/dev/null || { echo "${f}: not linked to libc++_shared.so"; return 1; }
    "${readelf}" -dW "${f}" | grep 'NEEDED.*libibmw_shm_transport' >/dev/null && { echo "${f}: links the plugin"; return 1; }
  done
  return 0
}

# HAP_BUNDLE goes into device shell commands: only a plain bundle name.
hap_bundle_ok() {
  [[ "${HAP_BUNDLE}" =~ ^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$ ]] || { fail "bad HAP_BUNDLE ${HAP_BUNDLE}"; return 1; }
  [[ "${HAP_SIGN}" == debug || "${HAP_SIGN}" == release ]] || { fail "bad HAP_SIGN ${HAP_SIGN}"; return 1; }
}

stage_hap-build() {
  [[ -f "${PLUGIN_SO}" && -d "${DYN}/test" ]] || { fail "hap-build: run dyn-build first"; return 1; }
  hap_bundle_ok || return 1
  # Added to the dyn-build trees and kept in their caches; the option only
  # adds targets, later dyn-build runs build what they built before.
  local t
  for t in rt test; do
    cmake -S "${DYN}/src" -B "${DYN}/${t}" -DIBMW_BUILD_SHM_EXT_HAP_CHECK=ON >"${LOGS}/hap-${t}.configure.log" 2>&1 ||
      { tail -20 "${LOGS}/hap-${t}.configure.log"; return 1; }
  done
  dyn_make rt ibmw_extensions_shm_ext ibmw_shm_ext_hap ibmw_shm_ext_hap_runner || return 1
  dyn_make test ibmw_extensions_shm_ext_hap_test || return 1
  local lib="${HAP}/stage/libs/arm64-v8a" src="${HERE}/ohos/hap" stage="${HAP}/stage"
  rm -rf "${HAP}" && mkdir -p "${lib}" "${stage}/js" "${stage}/res" "${stage}/abc" "${stage}/src/resources/base/media"
  cp "${PLUGIN_SO}" "${LIBCXX_SHARED}" "${DYN}/rt/libibmw_shm_ext_hap.so" "${DYN}/rt/libibmw_shm_ext_hap_runner.so" \
    "${DYN}/test/libibmw_extensions_shm_ext_hap_test.so" "${lib}/" || return 1
  hap_check_libs "${lib}/libibmw_shm_transport.so" "${lib}/libibmw_shm_ext_hap.so" "${lib}/libibmw_shm_ext_hap_runner.so" \
    "${lib}/libibmw_extensions_shm_ext_hap_test.so" || return 1
  # The NAPI module must not link libchild_process.so (see shm_ext_hap.cc).
  "${SDK}/llvm/bin/llvm-readelf" -dW "${lib}/libibmw_shm_ext_hap.so" | grep -q 'NEEDED.*libchild_process' &&
    { echo "libibmw_shm_ext_hap.so links libchild_process.so"; return 1; }
  # ArkTS (plain JS) to one merged abc, as the IDE's build does.
  cp -r "${src}/resources" "${stage}/src/"
  local debug=true
  [[ "${HAP_SIGN}" == release ]] && debug=false
  sed -e "s/@BUNDLE@/${HAP_BUNDLE}/g" -e "s/@DEBUG@/${debug}/" "${src}/module.json" >"${stage}/src/module.json"
  sed "s/@BUNDLE@/${HAP_BUNDLE}/g" "${src}/pack.info" >"${stage}/pack.info"
  sed "s/@BUNDLE@/${HAP_BUNDLE}/g" "${src}/OpenHarmonyTestRunner.js" >"${stage}/js/OpenHarmonyTestRunner.js"
  cp "${src}/EntryAbility.js" "${stage}/js/"
  # A plain 64x64 icon, generated rather than kept as a binary file.
  python3 - "${stage}/src/resources/base/media/app_icon.png" <<'EOF' || return 1
import struct, sys, zlib
w = h = 64
raw = b"".join(b"\0" + b"\x40\x80\xc0" * w for _ in range(h))
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
EOF
  # filesInfo: <file>;<record>;esm;<source>;<package>;<shared>;<language>
  {
    echo "${stage}/js/EntryAbility.js;${HAP_BUNDLE}/entry/ets/entryability/EntryAbility;esm;entry/src/main/ets/entryability/EntryAbility.ets;entry;false;js"
    echo "${stage}/js/OpenHarmonyTestRunner.js;${HAP_BUNDLE}/entry/ets/testrunner/OpenHarmonyTestRunner;esm;entry/src/main/ets/testrunner/OpenHarmonyTestRunner.ets;entry;false;js"
  } >"${stage}/filesInfo.txt"
  : >"${stage}/npmEntries.txt"
  "${SDK_ROOT}/ets/build-tools/ets-loader/bin/ark/build/bin/es2abc" "@${stage}/filesInfo.txt" \
    --npm-module-entry-list "${stage}/npmEntries.txt" --output "${stage}/abc/modules.abc" --merge-abc \
    --target-api-version=13 --module >"${LOGS}/hap-es2abc.log" 2>&1 || { cat "${LOGS}/hap-es2abc.log"; return 1; }
  "${SDK_ROOT}/toolchains/restool" -i "${stage}/src" -j "${stage}/src/module.json" -p "${HAP_BUNDLE}" -o "${stage}/res" \
    -r "${stage}/res/ResourceTable.h" -f >"${LOGS}/hap-restool.log" 2>&1 || { cat "${LOGS}/hap-restool.log"; return 1; }
  java -jar "${SIGN_LIB}/app_packing_tool.jar" --mode hap --json-path "${stage}/res/module.json" \
    --resources-path "${stage}/res/resources" --index-path "${stage}/res/resources.index" \
    --pack-info-path "${stage}/pack.info" --ets-path "${stage}/abc" --lib-path "${stage}/libs" \
    --out-path "${HAP}/unsigned.hap" --force true >"${LOGS}/hap-pack.log" 2>&1 || { cat "${LOGS}/hap-pack.log"; return 1; }
  unzip -l "${HAP}/unsigned.hap" | tee "${LOGS}/hap-contents.txt"
  echo "${HAP_SIGN}" >"${HAP}/sign-mode"
  echo "hap-build OK: ${HAP}/unsigned.hap (${HAP_SIGN})"
}

# hap_sign <udid>: OUT/hap/signed.hap with the SDK's OpenHarmony test keys. The
# profile's development (debug) or distribution (release) certificate is the
# one of the SDK template (issued by the Application CA); the app certificate
# chain is that leaf plus the CA and root certificates of OpenHarmony.p12. Only
# the debug profile names the device.
hap_sign() {
  local sign="${HAP}/sign" ks="${SIGN_LIB}/OpenHarmony.p12" template=Debug pkey=debug pcert=Debug
  [[ "${HAP_SIGN}" == release ]] && template=Released pkey=release pcert=Release
  rm -rf "${sign}" "${HAP}/signed.hap"
  mkdir -p "${sign}"
  python3 - "${SIGN_LIB}/Unsgned${template}ProfileTemplate.json" "${sign}" "${HAP_BUNDLE}" "$1" <<'EOF' || return 1
import json, sys, time, uuid
template, out, bundle, udid = sys.argv[1:]
p = json.load(open(template))
now = int(time.time())
p["uuid"] = str(uuid.uuid4())
p["validity"] = {"not-before": now - 86400, "not-after": now + 30 * 86400}
p["bundle-info"]["bundle-name"] = bundle
if p["type"] == "debug":
    p["debug-info"] = {"device-ids": [udid], "device-id-type": "udid"}
    cert = p["bundle-info"]["development-certificate"]
else:
    cert = p["bundle-info"]["distribution-certificate"]
open(out + "/profile.json", "w").write(json.dumps(p, indent=1))
open(out + "/app_chain.cer", "w").write(cert if cert.endswith("\n") else cert + "\n")
EOF
  local a
  for a in cacert rootcacert; do
    keytool -exportcert -rfc -alias "${a}" -keystore "${ks}" -storepass 123456 >>"${sign}/app_chain.cer" 2>/dev/null || return 1
  done
  java -jar "${SIGN_LIB}/hap-sign-tool.jar" sign-profile -keyAlias "openharmony application profile ${pkey}" \
    -signAlg SHA256withECDSA -mode localSign -profileCertFile "${SIGN_LIB}/OpenHarmonyProfile${pcert}.pem" \
    -inFile "${sign}/profile.json" -keystoreFile "${ks}" -outFile "${sign}/profile.p7b" -keyPwd 123456 \
    -keystorePwd 123456 >"${LOGS}/hap-sign.log" 2>&1 || { cat "${LOGS}/hap-sign.log"; return 1; }
  java -jar "${SIGN_LIB}/hap-sign-tool.jar" sign-app -keyAlias "openharmony application release" \
    -signAlg SHA256withECDSA -mode localSign -appCertFile "${sign}/app_chain.cer" -profileFile "${sign}/profile.p7b" \
    -inFile "${HAP}/unsigned.hap" -keystoreFile "${ks}" -outFile "${HAP}/signed.hap" -keyPwd 123456 \
    -keystorePwd 123456 -signCode 1 >>"${LOGS}/hap-sign.log" 2>&1 || { cat "${LOGS}/hap-sign.log"; return 1; }
  grep -q 'sign-app success' "${LOGS}/hap-sign.log" || { cat "${LOGS}/hap-sign.log"; return 1; }
}

# hap_state: prints "installed", "absent" or, when the device does not answer
# (hdc failed), "unknown"; only an answering device's bundle list counts.
hap_state() {
  local out
  out="$(hdc shell "echo hdc_ok; bm dump -a" 2>/dev/null | tr -d '\r')"
  if ! grep -qx 'hdc_ok' <<<"${out}" || ! grep -q '^ID: ' <<<"${out}"; then
    echo unknown
  elif grep -qxF "$(printf '\t%s' "${HAP_BUNDLE}")" <<<"${out}"; then
    echo installed
  else
    echo absent
  fi
}

# Uninstalls HAP_BUNDLE; succeeds only if the device then lists it as absent.
hap_uninstall() {
  hdc shell "bm uninstall -n ${HAP_BUNDLE}" >/dev/null 2>&1
  [[ "$(hap_state)" == absent ]]
}

# Whatever ends the script while the bundle may be installed (error, signal,
# lost terminal) uninstalls it first.
HAP_PENDING=0
hap_exit() {
  if [[ ${HAP_PENDING} -eq 1 ]]; then
    hap_uninstall || echo "FAIL: ${HAP_BUNDLE} may still be installed; run: hdc shell bm uninstall -n ${HAP_BUNDLE}" >&2
    HAP_PENDING=0
  fi
}

stage_hap-device() {
  hap_bundle_ok || return 1
  hdc list targets | grep -qv '\[Empty\]' || { fail "no hdc device"; return 1; }
  [[ -f "${HAP}/unsigned.hap" ]] || { fail "hap-device: run hap-build first"; return 1; }
  [[ "$(cat "${HAP}/sign-mode" 2>/dev/null)" == "${HAP_SIGN}" ]] ||
    { fail "hap-device: the HAP was not built with HAP_SIGN=${HAP_SIGN}; run hap-build again"; return 1; }
  local udid runs log uid ctx f
  udid="$(hdc shell "bm get --udid" | tr -d '\r' | grep -E '^[0-9A-F]{64}$')"
  [[ -n "${udid}" ]] || { fail "hap-device: no device UDID"; return 1; }
  hap_sign "${udid}" || { fail "hap-device: signing"; return 1; }
  trap hap_exit EXIT
  trap 'exit 130' INT TERM HUP
  HAP_PENDING=1
  case "$(hap_state)" in
    installed)
      # An earlier run's install keeps its data (cache, stale results): start clean.
      echo "NOTE: ${HAP_BUNDLE} is already installed (an earlier run?); uninstalling it first"
      hap_uninstall || { fail "hap-device: cannot uninstall the earlier ${HAP_BUNDLE}"; return 1; } ;;
    absent) ;;
    *) fail "hap-device: device does not answer"; return 1 ;;
  esac
  runs="${LOGS}/hap-$([[ "${HAP_SIGN}" == release ]] && echo release-)$(date +%Y%m%d-%H%M%S)"
  mkdir -p "${runs}"
  hdc install "${HAP}/signed.hap" | tr -d '\r' | tee "${runs}/install.log"
  grep -q 'install bundle successfully' "${runs}/install.log" || { fail "hap-device: install"; hap_exit; return 1; }
  hdc shell "bm dump -n ${HAP_BUNDLE}" | tr -d '\r' >"${runs}/bundle.json"
  uid="$(sed -n 's/^ *"uid": \([0-9]*\),*$/\1/p' "${runs}/bundle.json" | head -1)"
  echo "bundle uid=${uid}"
  log="${runs}/aa-test.log"
  # Native steps: TEST_TIMEOUT each for the two EXT runs, 300 s each for the
  # runner's two waits, plus the exit waits.
  hdc shell "aa test -b ${HAP_BUNDLE} -m entry -s unittest /ets/testrunner/OpenHarmonyTestRunner -s repeat ${REPEAT} \
    -s steptimeout ${TEST_TIMEOUT} -s timeout $(((TEST_TIMEOUT * 2 + 700) * 1000))" | tr -d '\r' | tee "${log}"
  # The sandbox cache directory as root sees it (el2, user 100 only: `aa test`
  # runs the current user's install).
  local cache="/data/app/el2/100/base/${HAP_BUNDLE}/cache"
  [[ "$(hdc shell "ls -d ${cache}" | tr -d '\r')" == "${cache}" ]] || fail "hap-device: no ${cache}"
  for f in summary.txt ext-once.log ext-once.rc ext-repeat.log ext-repeat.rc runner-pub.log runner-pub.rc \
    runner-sub.log runner-sub.rc; do
    # A child that died leaves no .rc: an empty file here.
    hdc shell "cat ${cache}/${f} 2>/dev/null" | tr -d '\r' >"${runs}/${f}"
  done
  if hap_uninstall; then
    HAP_PENDING=0
  else
    fail "hap-device: ${HAP_BUNDLE} still installed or device not answering"
  fi
  trap - INT TERM HUP

  grep -q '^TestFinished-ResultCode: 0$' "${log}" && grep -q '^HAP_RESULT result=PASS$' "${runs}/summary.txt" ||
    fail "hap-device: aa test (${log})"
  [[ "${uid}" =~ ^[1-9][0-9]{7}$ ]] || fail "hap-device: bundle uid '${uid}'"
  # Every child: the application uid/gid for all ids and the HAP domain (not su).
  # Supplementary groups and capabilities are the application's own (0x0a here),
  # so they are logged, not checked.
  # The installed bundle is what was asked for (provision type, debug flag).
  local ptype=debug debug=true
  ctx="u:r:debug_hap:s0"
  [[ "${HAP_SIGN}" == release ]] && ptype=release debug=false ctx="u:r:normal_hap:s0"
  grep -qx " *\"appProvisionType\": \"${ptype}\"," "${runs}/bundle.json" || fail "hap-device: appProvisionType is not ${ptype}"
  grep -qx " *\"debug\": ${debug}," "${runs}/bundle.json" || fail "hap-device: debug is not ${debug}"
  for f in ext-once ext-repeat runner-pub runner-sub; do
    grep -q "^HAP_CHILD pid=[0-9]* context=${ctx} " "${runs}/${f}.log" || fail "hap-device ${f}: not in ${ctx}"
    grep '^PROC_CREDS ' "${runs}/${f}.log" | grep -vE "^PROC_CREDS bin=[a-z_]+ uid=${uid},${uid},${uid},${uid} gid=${uid},${uid},${uid},${uid} " &&
      fail "hap-device ${f}: ids other than ${uid}"
    grep -q '^PROC_CREDS ' "${runs}/${f}.log" || fail "hap-device ${f}: no PROC_CREDS"
    # One C++ runtime (see shm_ext_hap.cc): the standard typeinfo of the child's
    # own code is libc++_shared.so's, and a typed catch works.
    grep -qE '^HAP_ABI self typeid\(std::exception\)=/[^ ]*/libc\+\+_shared\.so typeid\(std::runtime_error\)=/[^ ]*/libc\+\+_shared\.so$' \
      "${runs}/${f}.log" && grep -q '^HAP_EH typed_catch=1$' "${runs}/${f}.log" || fail "hap-device ${f}: C++ runtime"
  done
  # Measured, not gated: whether fstat on a memfd is denied in the HAP domain
  # (selfdev MapSegment then sizes segments with lseek). Gated: that fallback
  # relies on F_GET_SEALS failing on files other than shmem ones.
  grep -h '^HAP_MEMFD ' "${runs}/ext-once.log" | head -2
  grep -q '^HAP_SEALS pipe=-1 dir=-1 devnull=-1 file=-1$' "${runs}/ext-once.log" ||
    fail "hap-device: F_GET_SEALS works on a pipe, directory, /dev/null or regular file"
  for f in ext-once ext-repeat; do
    grep -qE '^HAP_ABI test typeid\(std::exception\)=/[^ ]*/libc\+\+_shared\.so ' "${runs}/${f}.log" ||
      fail "hap-device ${f}: the tests' typeinfo is not libc++_shared.so's"
    summarize "hap-${f}" "$(grep -xE -- '-?[0-9]+' "${runs}/${f}.rc" || echo -2)" "${runs}/${f}.log" ||
      fail "hap-device ${f} (${runs}/${f}.log)"
    [[ "$(grep -c '^\[  PASSED  \] 58 tests\.$' "${runs}/${f}.log")" -eq "$([[ ${f} == ext-once ]] && echo 1 || echo "${REPEAT}")" ]] ||
      fail "hap-device ${f}: expected 58 tests passed per iteration"
  done
  cat "${runs}/runner-pub.log" "${runs}/runner-sub.log" >"${runs}/runner.log"
  grep -E '^(HAP_CHILD|PROC_CREDS|DYN_)' "${runs}/runner.log" | grep -v '^DYN_ABI .*result=PASS$'
  if [[ "$(head -1 "${runs}/runner-pub.rc")" != 0 || "$(head -1 "${runs}/runner-sub.rc")" != 0 ]] ||
    [[ "$(grep -c '^DYN_CYCLE .*result=PASS$' "${runs}/runner.log")" -ne 4 ]] ||
    [[ "$(grep -c '^DYN_THROW .*result=PASS$' "${runs}/runner.log")" -ne 2 ]] || grep -q '^DYN_ABI .*result=FAIL$' "${runs}/runner.log" ||
    ! grep -q '^DYN_RESULT role=pub cycles=2 result=PASS' "${runs}/runner.log" ||
    ! grep -q '^DYN_RESULT role=sub cycles=2 result=PASS' "${runs}/runner.log"; then
    fail "hap-device runner (${runs}/runner.log)"
  fi
  echo "hap logs: ${runs}"
}

stage_hap() {
  hap_bundle_ok || return 1
  stage_dyn-build || return 1
  stage_hap-build || return 1
  stage_hap-device
}

stage_hap-release() {
  local HAP_SIGN=release
  stage_hap
}

[[ $# -eq 0 ]] && set -- build qemu
for s in "$@"; do
  echo "==== [shm-ohos] ${s} ===="
  case "${s}" in
    build | qemu | device | bench | bench-ext | clean | dynamic | dyn-build | dyn-device | dyn-uid | dyn-bench | dyn-clean | \
      hap | hap-build | hap-device | hap-release)
      "stage_${s}" || fail "stage ${s}" ;;
    *) echo "unknown stage ${s}" >&2; exit 64 ;;
  esac
done
exit "${rc}"
