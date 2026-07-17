#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_aarch64_dds_source_prefix.sh — validate a source-built AArch64 DDS prefix.
#
# This helper is intentionally DDS-only. It validates the source-built prefix used
# by cross builds before IBMW links against it; it does not require ROS 2 message
# packages and must not use host /opt/ros/humble as target evidence.
#
# Usage:
#   bash scripts/deps/check_aarch64_dds_source_prefix.sh <prefix> [toolchain-file]
#
# Examples:
#   bash scripts/deps/check_aarch64_dds_source_prefix.sh /tmp/aarch64-dds-prefix
#   bash scripts/deps/check_aarch64_dds_source_prefix.sh /tmp/aarch64-dds-prefix cmake/toolchains/aarch64-linux-gnu.cmake

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  check_aarch64_dds_source_prefix.sh <prefix> [toolchain-file]

Validates a source-built AArch64 DDS dependency prefix by running a clean CMake
configure probe with the AArch64 toolchain and the explicit prefix only.

Required target-discoverable packages:
  fastcdr
  fastdds or fastrtps
  tinyxml2
  foonathan_memory

The checker also rejects host leakage in CMake probe artifacts and verifies that
libraries found under the prefix are AArch64 objects where file/readelf can
inspect them.
EOF
}

fail() {
  echo "[aarch64-dds-prefix] ERROR: $*" >&2
  exit 1
}

note() {
  echo "[aarch64-dds-prefix] $*"
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage >&2
  exit 64
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TARGET_PREFIX="$1"
TOOLCHAIN_FILE="${2:-$REPO_ROOT/cmake/toolchains/aarch64-linux-gnu.cmake}"

if [[ ! -d "$TARGET_PREFIX" ]]; then
  echo "[aarch64-dds-prefix] ERROR: target prefix not found: $TARGET_PREFIX" >&2
  exit 64
fi

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
  echo "[aarch64-dds-prefix] ERROR: AArch64 toolchain file not found: $TOOLCHAIN_FILE" >&2
  exit 64
fi

TARGET_PREFIX="$(cd "$TARGET_PREFIX" && pwd)"
TOOLCHAIN_FILE="$(cd "$(dirname "$TOOLCHAIN_FILE")" && pwd)/$(basename "$TOOLCHAIN_FILE")"

case "$TARGET_PREFIX" in
  /opt/ros/humble|/opt/ros/humble/*)
    fail "host /opt/ros/humble must not be used as target DDS source prefix"
    ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
  fail "cmake is required"
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ibmw_aarch64_dds_prefix.XXXXXX")"
cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

PROBE_SRC="$BUILD_DIR/src"
PROBE_BUILD="$BUILD_DIR/build"
mkdir -p "$PROBE_SRC" "$PROBE_BUILD"

cat >"$PROBE_SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(ibmw_aarch64_dds_source_prefix_probe LANGUAGES NONE)

if(NOT DEFINED IBMW_DDS_SOURCE_PREFIX OR IBMW_DDS_SOURCE_PREFIX STREQUAL "")
  message(FATAL_ERROR "IBMW_DDS_SOURCE_PREFIX is required")
endif()

file(REAL_PATH "${IBMW_DDS_SOURCE_PREFIX}" _ibmw_prefix)

set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)

function(ibmw_assert_under_prefix package_name)
  foreach(var_name IN ITEMS
      ${package_name}_DIR
      ${package_name}_CONFIG
      ${package_name}_CONSIDERED_CONFIGS)
    if(DEFINED ${var_name})
      foreach(path_item IN LISTS ${var_name})
        if(path_item MATCHES "^/opt/ros/humble(/|$)")
          message(FATAL_ERROR
            "${package_name} resolved to host ROS 2 Humble path: ${path_item}")
        endif()
        if(NOT path_item MATCHES "^${_ibmw_prefix}(/|$)")
          message(FATAL_ERROR
            "${package_name} resolved outside target prefix '${_ibmw_prefix}': ${path_item}")
        endif()
      endforeach()
    endif()
  endforeach()
endfunction()

find_package(fastcdr CONFIG REQUIRED
  PATHS "${_ibmw_prefix}"
  NO_DEFAULT_PATH)
ibmw_assert_under_prefix(fastcdr)

find_package(fastdds CONFIG QUIET
  PATHS "${_ibmw_prefix}"
  NO_DEFAULT_PATH)
if(fastdds_FOUND)
  set(IBMW_DDS_IMPL_PACKAGE "fastdds")
  ibmw_assert_under_prefix(fastdds)
else()
  find_package(fastrtps CONFIG REQUIRED
    PATHS "${_ibmw_prefix}"
    NO_DEFAULT_PATH)
  set(IBMW_DDS_IMPL_PACKAGE "fastrtps")
  ibmw_assert_under_prefix(fastrtps)
endif()

find_package(tinyxml2 CONFIG REQUIRED
  PATHS "${_ibmw_prefix}"
  NO_DEFAULT_PATH)
ibmw_assert_under_prefix(tinyxml2)

find_package(foonathan_memory CONFIG REQUIRED
  PATHS "${_ibmw_prefix}"
  NO_DEFAULT_PATH)
ibmw_assert_under_prefix(foonathan_memory)

message(STATUS "IBMW_DDS_PREFIX=${_ibmw_prefix}")
message(STATUS "IBMW_DDS_IMPL_PACKAGE=${IBMW_DDS_IMPL_PACKAGE}")
message(STATUS "IBMW_FASTCDR_DIR=${fastcdr_DIR}")
message(STATUS "IBMW_TINYXML2_DIR=${tinyxml2_DIR}")
message(STATUS "IBMW_FOONATHAN_MEMORY_DIR=${foonathan_memory_DIR}")
if(fastdds_FOUND)
  message(STATUS "IBMW_FASTDDS_DIR=${fastdds_DIR}")
else()
  message(STATUS "IBMW_FASTRTPS_DIR=${fastrtps_DIR}")
endif()
EOF

note "running clean CMake configure probe"
cmake -S "$PROBE_SRC" -B "$PROBE_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_PREFIX_PATH="$TARGET_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$TARGET_PREFIX" \
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE \
  -DIBMW_DDS_SOURCE_PREFIX="$TARGET_PREFIX" \
  >"$BUILD_DIR/cmake_configure.log" 2>&1 || {
    cat "$BUILD_DIR/cmake_configure.log" >&2
    fail "CMake probe failed"
  }

declare -a PROBE_ARTIFACTS=("$BUILD_DIR/cmake_configure.log")
if [[ -f "$PROBE_BUILD/CMakeCache.txt" ]]; then
  PROBE_ARTIFACTS+=("$PROBE_BUILD/CMakeCache.txt")
fi
if [[ -d "$PROBE_BUILD/CMakeFiles" ]]; then
  while IFS= read -r -d '' log_file; do
    PROBE_ARTIFACTS+=("$log_file")
  done < <(find "$PROBE_BUILD/CMakeFiles" -type f \( -name 'CMake*.log' -o -name '*.log' \) -print0)
fi

if grep -E '/opt/ros/humble|(^|[^A-Za-z0-9_])(x86_64|amd64)([^A-Za-z0-9_]|$)' "${PROBE_ARTIFACTS[@]}" >/tmp/ibmw_aarch64_dds_prefix_leak.$$ 2>/dev/null; then
  cat /tmp/ibmw_aarch64_dds_prefix_leak.$$ >&2
  rm -f /tmp/ibmw_aarch64_dds_prefix_leak.$$
  fail "host leakage detected in CMake probe artifacts"
fi
rm -f /tmp/ibmw_aarch64_dds_prefix_leak.$$

declare -a LIB_CANDIDATES=()
for lib_dir in "$TARGET_PREFIX/lib" "$TARGET_PREFIX/lib64" "$TARGET_PREFIX/lib/aarch64-linux-gnu"; do
  if [[ -d "$lib_dir" ]]; then
    while IFS= read -r -d '' lib_path; do
      LIB_CANDIDATES+=("$lib_path")
    done < <(find "$lib_dir" -maxdepth 1 -type f \( \
      -name 'libfastcdr.so*' -o \
      -name 'libfastdds.so*' -o \
      -name 'libfastrtps.so*' -o \
      -name 'libtinyxml2.so*' -o \
      -name 'libfoonathan_memory*.so*' -o \
      -name 'libfastcdr.a' -o \
      -name 'libfastdds.a' -o \
      -name 'libfastrtps.a' -o \
      -name 'libtinyxml2.a' -o \
      -name 'libfoonathan_memory*.a' \
    \) -print0)
  fi
done

if (( ${#LIB_CANDIDATES[@]} == 0 )); then
  note "no matching library files found under prefix; CMake package discovery passed"
elif command -v file >/dev/null 2>&1; then
  for lib_path in "${LIB_CANDIDATES[@]}"; do
    file_output="$(file -L "$lib_path")"
    case "$file_output" in
      *"ARM aarch64"*|*"ARM64"*|*"current ar archive"*)
        ;;
      *"x86-64"*|*"x86_64"*|*"80386"*|*"Intel"*|*"AMD"*)
        fail "non-AArch64 host library detected: $file_output"
        ;;
      *)
        note "warning: unable to classify library architecture with file: $file_output"
        ;;
    esac
  done
else
  note "warning: file command not available; skipping library architecture check"
fi

if command -v readelf >/dev/null 2>&1; then
  for lib_path in "${LIB_CANDIDATES[@]}"; do
    if [[ "$lib_path" == *.a ]]; then
      continue
    fi
    if readelf_header="$(readelf -h "$lib_path" 2>/dev/null)"; then
      if ! grep -q 'Machine:[[:space:]]*AArch64' <<<"$readelf_header"; then
        fail "readelf reports non-AArch64 shared library: $lib_path"
      fi
    fi
  done
else
  note "warning: readelf command not available; skipping ELF header architecture check"
fi

note "PASS: source-built DDS prefix is target-discoverable and free of host leakage."
