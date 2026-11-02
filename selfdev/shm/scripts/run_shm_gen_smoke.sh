#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# ibmw_gen smoke test of the shm transport: generates the loaned-shm and the
# direct-shm project of selfdev/shm/test/gen/*.ibmw.yaml, builds both against
# an installed ROS2-enabled IBMW, runs each publisher/subscriber pair briefly
# on a fresh IBMW_SHM_ROOT and checks that the subscriber receives messages.
#
# Usage: run_shm_gen_smoke.sh
#
# Environment:
#   IBMW_PREFIX      installed IBMW to build against. Default: configure,
#                    build and install a ROS2-enabled IBMW (the options of
#                    build-gencheck) into $BUILD_ROOT/ibmw-install.
#   ROS2_PREFIX      ROS 2 installation          (default: /opt/ros/humble)
#   BUILD_ROOT       work directory              (default: <repo>/build-shm-gen)
#   IBMW_CMAKE_ARGS  extra cmake arguments of the IBMW build (default: empty)
#   JOBS             build parallelism           (default: nproc)
#   RUN_SECONDS      how long each pair runs     (default: 4)
#   BUILD_TIMEOUT    limit of one configure/build step, seconds (default: 3600)
#   CMAKE_GENERATOR  cmake generator             (default: Ninja if installed)
#
# Exit status: 0 pass, 1 failure, 77 skipped (no ROS 2 installation).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
GEN_DIR="${REPO_DIR}/selfdev/shm/test/gen"
ROS2_PREFIX="${ROS2_PREFIX:-/opt/ros/humble}"
BUILD_ROOT="${BUILD_ROOT:-${REPO_DIR}/build-shm-gen}"
JOBS="${JOBS:-$(nproc)}"
RUN_SECONDS="${RUN_SECONDS:-4}"
BUILD_TIMEOUT="${BUILD_TIMEOUT:-3600}"
read -r -a EXTRA_ARGS <<<"${IBMW_CMAKE_ARGS:-}"

if [[ ! -d "${ROS2_PREFIX}" ]]; then
  echo "SKIP: ROS 2 not found at ${ROS2_PREFIX} (set ROS2_PREFIX); the generated projects need geometry_msgs."
  exit 77
fi
# ament's CMake helpers run Python that imports ament_package from the ROS
# environment (PYTHONPATH, AMENT_PREFIX_PATH).
if [[ -f "${ROS2_PREFIX}/setup.bash" ]]; then
  set +u
  # shellcheck disable=SC1091
  source "${ROS2_PREFIX}/setup.bash"
  set -u
fi

if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
  GENERATOR_ARG=(-G "${CMAKE_GENERATOR}")
elif command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARG=(-G Ninja)
else
  GENERATOR_ARG=()
fi

log() { printf '\n==== [shm_gen] %s ====\n' "$*"; }
fail() {
  echo "FAIL: $*"
  exit 1
}

# The shm root of the running pair, removed however the script ends.
CURRENT_ROOT=""
# The running build step (a timeout process leading its own process group).
STEP_PID=""
stop_step() {
  [[ -z "${STEP_PID}" ]] && return
  kill -TERM "${STEP_PID}" 2>/dev/null
  wait "${STEP_PID}" 2>/dev/null
  STEP_PID=""
}
trap '[[ -z "${CURRENT_ROOT}" ]] || rm -rf "${CURRENT_ROOT}"' EXIT
trap 'stop_step; exit 130' INT
trap 'stop_step; exit 143' TERM

# limited <command...>: run one configure/build step for at most
# BUILD_TIMEOUT seconds. timeout puts the step in its own process group, out
# of reach of a terminal Ctrl-C, so it runs in the background and an
# interrupt of the script stops it through stop_step.
limited() {
  timeout "${BUILD_TIMEOUT}" "$@" &
  STEP_PID=$!
  local rc=0
  wait "${STEP_PID}" || rc=$?
  STEP_PID=""
  return "${rc}"
}

mkdir -p "${BUILD_ROOT}"
BUILD_ROOT="$(cd "${BUILD_ROOT}" && pwd)"

# ------------------------------------------------------------ IBMW install
if [[ -z "${IBMW_PREFIX:-}" ]]; then
  IBMW_PREFIX="${BUILD_ROOT}/ibmw-install"
  log "configure+build+install IBMW into ${IBMW_PREFIX}"
  limited cmake -S "${REPO_DIR}" -B "${BUILD_ROOT}/ibmw" "${GENERATOR_ARG[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="${ROS2_PREFIX}" \
        -DIBMW_BUILD_RUNTIME=ON -DIBMW_INSTALL=ON -DIBMW_BUILD_ROS2_TYPES=ON -DIBMW_BUILD_SHM_EXTENSION=ON \
        -DIBMW_BUILD_CLI_TOOLS=ON -DIBMW_DDS_PROVIDER=SYSTEM -DIBMW_ENABLE_DLOPEN_DEEPBIND=ON \
        -DIBMW_USE_FMT_LIB=ON -DIBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES=ON \
        -DIBMW_BUILD_TESTS=OFF -DIBMW_BUILD_EXAMPLES=OFF -DIBMW_BUILD_PROTOCOLS=OFF \
        -DIBMW_BUILD_ICEORYX_EXTENSION=OFF -DIBMW_BUILD_DDS_EXTENSION=OFF -DIBMW_BUILD_WITH_ROS2=OFF \
        -DIBMW_BUILD_WITH_WERROR=OFF "${EXTRA_ARGS[@]}" \
        >"${BUILD_ROOT}/ibmw.configure.log" 2>&1 ||
    { tail -40 "${BUILD_ROOT}/ibmw.configure.log"; fail "IBMW configure (see ${BUILD_ROOT}/ibmw.configure.log)"; }
  limited cmake --build "${BUILD_ROOT}/ibmw" -j "${JOBS}" >"${BUILD_ROOT}/ibmw.build.log" 2>&1 ||
    { tail -40 "${BUILD_ROOT}/ibmw.build.log"; fail "IBMW build (see ${BUILD_ROOT}/ibmw.build.log)"; }
  rm -rf "${IBMW_PREFIX}"
  cmake --install "${BUILD_ROOT}/ibmw" --prefix "${IBMW_PREFIX}" >"${BUILD_ROOT}/ibmw.install.log" 2>&1 ||
    { tail -40 "${BUILD_ROOT}/ibmw.install.log"; fail "IBMW install (see ${BUILD_ROOT}/ibmw.install.log)"; }
fi
[[ -f "${IBMW_PREFIX}/lib/cmake/ibmw/ibmw-config.cmake" ]] || fail "no IBMW install at IBMW_PREFIX=${IBMW_PREFIX}"
echo "IBMW prefix: ${IBMW_PREFIX}"

# ----------------------------------------------------- generate and build
# gen_build <name> <yaml> <extra cxx flags>
gen_build() {
  local name=$1 yaml=$2 cxx=$3
  local out="${BUILD_ROOT}/${name}_gen" bld="${BUILD_ROOT}/${name}_build"
  log "generate+build ${name}"
  rm -rf "${out}" "${bld}"
  python3 "${REPO_DIR}/src/tools/ibmw_gen/ibmw_gen.py" "${yaml}" -o "${out}" >"${BUILD_ROOT}/${name}.gen.log" 2>&1 ||
    { cat "${BUILD_ROOT}/${name}.gen.log"; fail "ibmw_gen ${name}"; }
  limited cmake -S "${out}" -B "${bld}" "${GENERATOR_ARG[@]}" -DCMAKE_PREFIX_PATH="${IBMW_PREFIX};${ROS2_PREFIX}" \
        -DCMAKE_CXX_FLAGS="${cxx}" >"${BUILD_ROOT}/${name}.configure.log" 2>&1 ||
    { tail -40 "${BUILD_ROOT}/${name}.configure.log"; fail "configure ${name} (see ${BUILD_ROOT}/${name}.configure.log)"; }
  limited cmake --build "${bld}" -j "${JOBS}" >"${BUILD_ROOT}/${name}.build.log" 2>&1 ||
    { tail -40 "${BUILD_ROOT}/${name}.build.log"; fail "build ${name} (see ${BUILD_ROOT}/${name}.build.log)"; }
}

# run_pair <name> <pub app> <sub app> <pattern the subscriber logs per message>
run_pair() {
  local name=$1 pub=$2 sub=$3 pattern=$4
  local bld="${BUILD_ROOT}/${name}_build" root
  log "run ${name}: ${sub} + ${pub} for ${RUN_SECONDS}s"
  root="$(mktemp -d "${TMPDIR:-/tmp}/ibmw_shm_gen.XXXXXX")"
  CURRENT_ROOT="${root}"
  (
    pub_pid="" sub_pid=""
    # shellcheck disable=SC2086  # the ids are unquoted so that empty ones vanish
    trap 'kill -KILL ${pub_pid} ${sub_pid} 2>/dev/null' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    cd "${bld}" || exit 1
    export IBMW_SHM_ROOT="${root}" LD_LIBRARY_PATH="${IBMW_PREFIX}/bin:${IBMW_PREFIX}/lib:${ROS2_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    unset IBMW_SHM_DOMAIN
    "./${sub}" "cfg/${sub}.yaml" >"${BUILD_ROOT}/${name}.sub.log" 2>&1 &
    sub_pid=$!
    "./${pub}" "cfg/${pub}.yaml" >"${BUILD_ROOT}/${name}.pub.log" 2>&1 &
    pub_pid=$!
    sleep "${RUN_SECONDS}"
    kill -INT "${pub_pid}" "${sub_pid}" 2>/dev/null
    local pub_rc=0 sub_rc=0
    for _ in $(seq 100); do
      kill -0 "${pub_pid}" 2>/dev/null || kill -0 "${sub_pid}" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "${pub_pid}" "${sub_pid}" 2>/dev/null
    wait "${pub_pid}" || pub_rc=$?
    wait "${sub_pid}" || sub_rc=$?
    pub_pid="" sub_pid=""
    echo "${pub} exit ${pub_rc}, ${sub} exit ${sub_rc}"
    [[ ${pub_rc} -eq 0 && ${sub_rc} -eq 0 ]]
  )
  local rc=$?
  rm -rf "${root}"
  CURRENT_ROOT=""
  local received
  received="$(grep -c -- "${pattern}" "${BUILD_ROOT}/${name}.sub.log")"
  echo "${name}: ${received} message(s) received"
  if [[ ${received} -eq 0 ]]; then
    tail -20 "${BUILD_ROOT}/${name}.pub.log" "${BUILD_ROOT}/${name}.sub.log"
    fail "${name}: the subscriber received nothing"
  fi
  if [[ ${rc} -ne 0 ]]; then
    tail -20 "${BUILD_ROOT}/${name}.pub.log" "${BUILD_ROOT}/${name}.sub.log"
    fail "${name}: an app did not exit cleanly on SIGINT"
  fi
}

gen_build loaned "${GEN_DIR}/loaned_shm.ibmw.yaml" ""
# Core issue C5: the installed transport_driver_tools.h (included through
# shm_ext/shm_transport_driver.h by the direct modules) includes
# observability/comm_trace.h, which is not installed. Take it from the source
# tree until the install rules carry it.
direct_cxx=""
if [[ ! -f "${IBMW_PREFIX}/include/ibmw/observability/comm_trace.h" ]] &&
   ! find "${IBMW_PREFIX}/include" -path '*/observability/comm_trace.h' -print -quit | grep -q .; then
  echo "WARN: core issue C5: ${IBMW_PREFIX} does not install observability/comm_trace.h;" \
       "the direct project is built with -isystem ${REPO_DIR}/src/core."
  direct_cxx="-isystem ${REPO_DIR}/src/core"
fi
gen_build direct "${GEN_DIR}/direct_shm.ibmw.yaml" "${direct_cxx}"

run_pair loaned loan_pub_app loan_sub_app "\[RECV\] point_topic"
run_pair direct direct_pub_app direct_sub_app "Received direct shm message"

log "PASS"
