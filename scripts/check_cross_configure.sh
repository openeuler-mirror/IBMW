#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_cross_configure.sh — validate the cross-aarch64-minimal configure path.
#
# This script keeps the slice intentionally small:
#   - configure the cross-aarch64-minimal preset into a temp build dir
#   - allow explicit compiler overrides, with auto-detection fallback for
#     aarch64-linux-gnu-gcc/g++ when available
#   - verify the expected cross defaults in CMake cache / probe output
#   - fail if the host ROS 2 Humble prefix leaks into the cache or logs
#
# Usage:
#   bash scripts/check_cross_configure.sh
#
# Exit codes:
#   0  success
#   1  validation failed
#   2  cmake missing or configure failed
#   64 CLI usage error

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PRESET_NAME="cross-aarch64-minimal"
TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchains/aarch64-linux-gnu.cmake"
HOST_ROS_PREFIX="/opt/ros/humble"
CROSS_TOOLCHAIN_BIN_DIR="${CROSS_TOOLCHAIN_BIN_DIR:-}"

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
  echo "[cross-config] ERROR: toolchain file not found: $TOOLCHAIN_FILE" >&2
  exit 2
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[cross-config] ERROR: cmake not found on PATH" >&2
  exit 2
fi

BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ibmw-cross-config.XXXXXX")"
PROJECT_BUILD="$BUILD_ROOT/project"
PROBE_BUILD="$BUILD_ROOT/probe"
PROBE_SOURCE="$BUILD_ROOT/probe-src"
PROJECT_LOG="$BUILD_ROOT/project-configure.log"
PROBE_LOG="$BUILD_ROOT/probe-configure.log"
cleanup() {
  rm -rf "$BUILD_ROOT"
}
trap cleanup EXIT

declare -a CMAKE_ARGS=(
  "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DIBMW_BUILD_WITH_ROS2=OFF"
  "-DIBMW_BUILD_ROS2_TYPES=OFF"
  "-DIBMW_TARGET_ROS2_TIER=none"
)

find_first_executable() {
  local candidate
  for candidate in "$@"; do
    if [[ "$candidate" == */* && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
    if [[ "$candidate" != */* ]] && command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

if [[ -n "${CROSS_CC:-}" ]]; then
  CMAKE_ARGS+=("-DCMAKE_C_COMPILER=${CROSS_CC}")
elif detected_cc="$(find_first_executable \
    aarch64-linux-gnu-gcc \
    aarch64-openeuler-linux-gnu-gcc \
    "${CROSS_TOOLCHAIN_BIN_DIR:+${CROSS_TOOLCHAIN_BIN_DIR}/aarch64-openeuler-linux-gnu-gcc}")"; then
  CMAKE_ARGS+=("-DCMAKE_C_COMPILER=${detected_cc}")
fi

if [[ -n "${CROSS_CXX:-}" ]]; then
  CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=${CROSS_CXX}")
elif detected_cxx="$(find_first_executable \
    aarch64-linux-gnu-g++ \
    aarch64-openeuler-linux-gnu-g++ \
    "${CROSS_TOOLCHAIN_BIN_DIR:+${CROSS_TOOLCHAIN_BIN_DIR}/aarch64-openeuler-linux-gnu-g++}")"; then
  CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=${detected_cxx}")
fi

echo "[cross-config] preset: ${PRESET_NAME}"
echo "[cross-config] repo:   ${REPO_ROOT}"
echo "[cross-config] build:  ${PROJECT_BUILD}"
echo "[cross-config] toolchain: ${TOOLCHAIN_FILE}"

cmake --preset "$PRESET_NAME" -B "$PROJECT_BUILD" "${CMAKE_ARGS[@]}" \
  >"$PROJECT_LOG" 2>&1 || {
    echo "[cross-config] ERROR: project configure failed" >&2
    echo "[cross-config] log: $PROJECT_LOG" >&2
    exit 2
  }

mkdir -p "$PROBE_SOURCE"
cat > "$PROBE_SOURCE/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(ibmw_cross_probe LANGUAGES C CXX)
message(STATUS "CMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}")
message(STATUS "CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
EOF

cmake -S "$PROBE_SOURCE" -B "$PROBE_BUILD" "${CMAKE_ARGS[@]}" \
  >"$PROBE_LOG" 2>&1 || {
    echo "[cross-config] ERROR: probe configure failed" >&2
    echo "[cross-config] log: $PROBE_LOG" >&2
    exit 2
  }

if grep -R -F "$HOST_ROS_PREFIX" "$BUILD_ROOT" >/dev/null 2>&1; then
  echo "[cross-config] ERROR: host ROS prefix leaked into configure outputs: $HOST_ROS_PREFIX" >&2
  echo "[cross-config] inspect: $BUILD_ROOT" >&2
  exit 1
fi

if ! grep -Fq 'CMAKE_CROSSCOMPILING=TRUE' "$PROBE_LOG"; then
  echo "[cross-config] ERROR: expected CMAKE_CROSSCOMPILING=TRUE in probe output" >&2
  echo "[cross-config] log: $PROBE_LOG" >&2
  exit 1
fi

if ! grep -Fq 'CMAKE_SYSTEM_PROCESSOR=aarch64' "$PROBE_LOG"; then
  echo "[cross-config] ERROR: expected CMAKE_SYSTEM_PROCESSOR=aarch64 in probe output" >&2
  echo "[cross-config] log: $PROBE_LOG" >&2
  exit 1
fi

if ! grep -Fq 'IBMW_BUILD_WITH_ROS2:BOOL=OFF' "$PROJECT_BUILD/CMakeCache.txt"; then
  echo "[cross-config] ERROR: IBMW_BUILD_WITH_ROS2 did not default to OFF" >&2
  exit 1
fi

if ! grep -Fq 'IBMW_BUILD_ROS2_TYPES:BOOL=OFF' "$PROJECT_BUILD/CMakeCache.txt"; then
  echo "[cross-config] ERROR: IBMW_BUILD_ROS2_TYPES did not default to OFF" >&2
  exit 1
fi

if ! grep -Fq 'IBMW_TARGET_ROS2_TIER:STRING=none' "$PROJECT_BUILD/CMakeCache.txt"; then
  echo "[cross-config] ERROR: IBMW_TARGET_ROS2_TIER did not default to none" >&2
  exit 1
fi

if grep -R -F "$HOST_ROS_PREFIX" "$PROJECT_BUILD/CMakeCache.txt" "$PROJECT_LOG" "$PROBE_LOG" >/dev/null 2>&1; then
  echo "[cross-config] ERROR: host ROS prefix leaked into cache or logs" >&2
  exit 1
fi

echo "[cross-config] PASS: cross-aarch64-minimal configure defaults validated without host ROS2 leakage."
