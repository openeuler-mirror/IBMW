#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# x86 A/B run of the shm_ext loan/publish microbenchmark (ibmw_shm_ext_bench):
# builds this tree and, when BENCH_BASE_SRC is set, a baseline tree with the same
# flags, then runs them interleaved (base new base new ...) and prints a summary.
# Both trees are copied to a scratch directory; the bench source and its CMake
# block are copied into a tree that does not have them. The device counterpart is
# `run_shm_ohos.sh bench-ext`.
#
# Environment:
#   BENCH_BASE_SRC   baseline source tree, e.g. a worktree of 89b5a1a (default: none)
#   BUILD_ROOT       scratch and logs (default: <repo>/build-shm-ext-bench)
#   DEPS_DIR         FetchContent sources (default: <repo>/../deps)
#   BENCH_ROUNDS     rounds per binary (default: 9)
#   BENCH_ARGS       extra bench arguments (default: --reps 5 --ms 100)
#   BENCH_CPUS       worker CPUs (default: 2,4,6,8,10,12,14,0, physical P-cores of an i7-14700)
#   BENCH_MASK       CPU mask of the process, listeners run there (default: 0-15)
#   JOBS             build parallelism (default: nproc)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${REPO}/build-shm-ext-bench}"
DEPS_DIR="${DEPS_DIR:-$(dirname "${REPO}")/deps}"
BENCH_BASE_SRC="${BENCH_BASE_SRC:-}"
BENCH_ROUNDS="${BENCH_ROUNDS:-9}"
BENCH_ARGS="${BENCH_ARGS:---reps 5 --ms 100}"
BENCH_CPUS="${BENCH_CPUS:-2,4,6,8,10,12,14,0}"
BENCH_MASK="${BENCH_MASK:-0-15}"
JOBS="${JOBS:-$(nproc)}"
LOGS="${BUILD_ROOT}/logs"
GENERATOR_ARG=()
command -v ninja >/dev/null && GENERATOR_ARG=(-G Ninja)

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DIBMW_BUILD_TESTS=OFF -DIBMW_BUILD_SHM_EXT_BENCH=ON
  -DIBMW_BUILD_SHM_EXTENSION=ON -DIBMW_BUILD_OHOS_CAPTURE_AGENT=OFF -DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=OFF
  -DIBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE=OFF -DIBMW_BUILD_CLI_TOOLS=OFF -DIBMW_BUILD_EXAMPLES=OFF
  -DIBMW_BUILD_PROTOCOLS=OFF -DIBMW_BUILD_ROS2_TYPES=OFF)
for e in ICEORYX DDS ECHO GRPC MQTT NET ZENOH PROXY PARAMETER LOG_CONTROL OPENTELEMETRY RECORD_PLAYBACK \
  SERVICE_INTROSPECTION TIME_MANIPULATOR TOPIC_LOGGER; do
  CMAKE_ARGS+=("-DIBMW_BUILD_${e}_EXTENSION=OFF")
done
for d in ASIO BACKWARD CPPTOML FMT GFLAGS GOOGLETEST ICEORYX JSONCPP LIBUNIFEX NLOHMANN_JSON TBB YAML-CPP; do
  CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${d}=${DEPS_DIR}/$(echo "${d}" | tr '[:upper:]' '[:lower:]')-src")
done

# build_tree <label> <source tree>
build_tree() {
  local label=$1 from=$2 src="${BUILD_ROOT}/${1}/src" dir="${BUILD_ROOT}/${1}/build"
  mkdir -p "${src}" "${LOGS}"
  rsync -a --delete --exclude .git --exclude .omc --exclude '/build*/' "${from}/" "${src}/" || return 1
  "${HERE}/shm_ext_bench_overlay.sh" "${REPO}" "${src}" || return 1
  cmake -S "${src}" -B "${dir}" "${GENERATOR_ARG[@]}" "${CMAKE_ARGS[@]}" >"${LOGS}/${label}.configure.log" 2>&1 ||
    { tail -20 "${LOGS}/${label}.configure.log"; return 1; }
  cmake --build "${dir}" -j "${JOBS}" --target ibmw_shm_ext_bench >"${LOGS}/${label}.build.log" 2>&1 ||
    { tail -40 "${LOGS}/${label}.build.log"; return 1; }
  echo "built ${label}: $(find "${dir}" -name ibmw_shm_ext_bench -type f -executable | head -1)"
}

build_tree new "${REPO}" || exit 1
LABELS=(new)
if [[ -n "${BENCH_BASE_SRC}" ]]; then
  build_tree base "${BENCH_BASE_SRC}" || exit 1
  LABELS=(base new)
fi

RUNS="${LOGS}/runs"
rm -rf "${RUNS}" && mkdir -p "${RUNS}"
root="$(mktemp -d /tmp/ibx.XXXXXX)"
trap 'rm -rf "${root}"' EXIT
read -r -a extra <<<"${BENCH_ARGS}"
rc=0
for ((round = 1; round <= BENCH_ROUNDS; ++round)); do
  for label in "${LABELS[@]}"; do
    bin="$(find "${BUILD_ROOT}/${label}/build" -name ibmw_shm_ext_bench -type f -executable | head -1)"
    log="${RUNS}/${label}.${round}.log"
    echo "round ${round} ${label} load: $(cut -d" " -f1 /proc/loadavg)"
    IBMW_SHM_ROOT="${root}" taskset -c "${BENCH_MASK}" "${bin}" --cpus "${BENCH_CPUS}" "${extra[@]}" >"${log}" 2>&1 ||
      { echo "FAIL: ${label} round ${round} (${log})"; rc=1; }
    find "${root}" -mindepth 1 -delete
  done
done
python3 "${HERE}/shm_ext_bench_summary.py" "${RUNS}" || rc=1
exit "${rc}"
