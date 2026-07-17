#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_ohos_cross_configure.sh — validate the cross-ohos-arm64-minimal configure path.
#
# This script keeps the slice intentionally small:
#   - configure the cross-ohos-arm64-minimal preset into a clean build dir
#   - verify the expected OHOS cross defaults in CMake cache / generated metadata
#   - fail if obvious host target-package paths leak into cache or logs
#   - allow host-side tools/programs (for example cmake or Python) by path
#
# Usage:
#   bash scripts/check_ohos_cross_configure.sh [build-dir]
#
# Exit codes:
#   0  success
#   1  validation failed
#   2  cmake missing or configure failed
#   64 CLI usage error

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/check_ohos_cross_configure.sh [build-dir]

Configures the cross-ohos-arm64-minimal preset into a clean build directory and
validates the expected OHOS cross defaults while rejecting obvious host target
package leakage from /opt/ros/humble and /usr/lib/x86_64-linux-gnu.

Host-side programs/tools such as cmake and Python are allowed to appear in logs.
EOF
}

fail() {
  echo "[ohos-cross-config] FAIL: $*" >&2
  exit 1
}

error_exit() {
  echo "[ohos-cross-config] FAIL: $*" >&2
  exit 2
}

note() {
  echo "[ohos-cross-config] $*"
}

pass() {
  echo "[ohos-cross-config] PASS: $*"
}

if [[ $# -gt 1 ]]; then
  usage >&2
  exit 64
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PRESET_NAME="cross-ohos-arm64-minimal"
DEFAULT_BUILD_DIR="$REPO_ROOT/build-ohos-arm64-minimal-check"
BUILD_DIR="${1:-$DEFAULT_BUILD_DIR}"
CONFIGURE_LOG="$BUILD_DIR/configure.log"
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
OHOS_SDK_NATIVE_ROOT_ARG="${OHOS_SDK_NATIVE_ROOT:-}"

case "$BUILD_DIR" in
  ""|"."|".."|"/"|"~"|"-"*)
    error_exit "refusing unsafe build dir: $BUILD_DIR"
    ;;
esac

case "$BUILD_DIR" in
  "$REPO_ROOT"/*|build-*|*/build-*)
    ;;
  *)
    error_exit "build dir must be a repo-local or explicit build-* path: $BUILD_DIR"
    ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
  error_exit "cmake not found on PATH"
fi

require_file_contains() {
  local file_path="$1"
  local expected="$2"
  local label="$3"

  if [[ ! -f "$file_path" ]]; then
    fail "$label file not found: $file_path"
  fi

  if ! grep -Fq "$expected" "$file_path"; then
    fail "expected $label entry '$expected' not found in $file_path"
  fi
}

require_cache_off() {
  local cache_name="$1"

  if [[ ! -f "$CACHE_FILE" ]]; then
    fail "cache file not found: $CACHE_FILE"
  fi

  if ! grep -Eq "^${cache_name}:(BOOL|STRING|INTERNAL)=OFF$" "$CACHE_FILE"; then
    fail "expected ${cache_name} to be OFF in $CACHE_FILE"
  fi
}

find_cmake_system_file() {
  local candidate
  while IFS= read -r candidate; do
    printf '%s\n' "$candidate"
    return 0
  done < <(compgen -G "$BUILD_DIR/CMakeFiles/*/CMakeSystem.cmake" || true)
  return 1
}

is_allowed_host_tool_line() {
  local line="$1"

  if [[ "$line" == *CMAKE_COMMAND* ||
        "$line" == *CMAKE_MAKE_PROGRAM* ||
        "$line" == *Python*_EXECUTABLE* ||
        "$line" == *PYTHON*_EXECUTABLE* ||
        "$line" == *Python_EXECUTABLE* ||
        "$line" == *PYTHON_EXECUTABLE* ||
        "$line" == *python* ||
        "$line" == *interpreter* ||
        "$line" == *_PROGRAM* ||
        "$line" == *" program "* ||
        "$line" == *" Program "* ||
        "$line" == *executable* ||
        "$line" == *Executable* ]]; then
    return 0
  fi

  return 1
}

check_forbidden_path_leakage() {
  local forbidden_path="$1"
  shift
  local artifact
  local line

  for artifact in "$@"; do
    [[ -f "$artifact" ]] || continue

    while IFS= read -r line; do
      if is_allowed_host_tool_line "$line"; then
        continue
      fi
      fail "forbidden target package path leaked into configure artifacts: $line"
    done < <(grep -Fn "$forbidden_path" "$artifact" || true)
  done
}

rm -rf -- "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

note "preset: ${PRESET_NAME}"
note "repo:   ${REPO_ROOT}"
note "build:  ${BUILD_DIR}"

cmake_args=(--preset "$PRESET_NAME" -B "$BUILD_DIR")
if [[ -n "$OHOS_SDK_NATIVE_ROOT_ARG" ]]; then
  cmake_args+=(-DOHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT_ARG")
fi

if ! cmake "${cmake_args[@]}" >"$CONFIGURE_LOG" 2>&1; then
  if grep -Fq 'OHOS toolchain file not found:' "$CONFIGURE_LOG"; then
    echo "[ohos-cross-config] FAIL: OHOS SDK/toolchain missing or invalid." >&2
    echo "[ohos-cross-config] hint: set OHOS_SDK_NATIVE_ROOT to your OpenHarmony SDK native directory." >&2
    echo "[ohos-cross-config] log: $CONFIGURE_LOG" >&2
    exit 2
  fi

  echo "[ohos-cross-config] FAIL: cmake configure failed for preset ${PRESET_NAME}." >&2
  echo "[ohos-cross-config] log: $CONFIGURE_LOG" >&2
  exit 2
fi

declare -a ARTIFACTS=("$CACHE_FILE" "$CONFIGURE_LOG")
if [[ -f "$BUILD_DIR/CMakeFiles/CMakeOutput.log" ]]; then
  ARTIFACTS+=("$BUILD_DIR/CMakeFiles/CMakeOutput.log")
fi
if [[ -f "$BUILD_DIR/CMakeFiles/CMakeError.log" ]]; then
  ARTIFACTS+=("$BUILD_DIR/CMakeFiles/CMakeError.log")
fi

require_file_contains "$CACHE_FILE" 'CMAKE_SYSTEM_PROCESSOR:STRING=aarch64' 'cache'
require_file_contains "$CACHE_FILE" 'OHOS_ARCH:STRING=arm64-v8a' 'cache'
require_file_contains "$CACHE_FILE" 'OHOS_PLATFORM:STRING=OHOS' 'cache'
require_file_contains "$CACHE_FILE" 'OHOS_STL:STRING=c++_shared' 'cache'
require_file_contains "$CACHE_FILE" 'IBMW_BUILD_PROTOCOLS:BOOL=OFF' 'cache'
require_file_contains "$CACHE_FILE" 'IBMW_BUILD_WITH_ROS2:BOOL=OFF' 'cache'
require_file_contains "$CACHE_FILE" 'IBMW_BUILD_ROS2_TYPES:BOOL=OFF' 'cache'
require_cache_off 'IBMW_BUILD_ROS2_EXTENSION'
require_file_contains "$CACHE_FILE" 'IBMW_TARGET_ROS2_TIER:STRING=none' 'cache'

if cmake_system_file="$(find_cmake_system_file)"; then
  require_file_contains "$cmake_system_file" 'set(CMAKE_CROSSCOMPILING "TRUE")' 'generated system metadata'
  require_file_contains "$cmake_system_file" 'set(CMAKE_SYSTEM_NAME "OHOS")' 'generated system metadata'
else
  fail "generated CMakeSystem.cmake not found under $BUILD_DIR/CMakeFiles"
fi

check_forbidden_path_leakage '/opt/ros/humble' "${ARTIFACTS[@]}"
check_forbidden_path_leakage '/usr/lib/x86_64-linux-gnu' "${ARTIFACTS[@]}"

pass 'cross-ohos-arm64-minimal configure defaults validated without obvious host target-package leakage.'
