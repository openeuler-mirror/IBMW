#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_ohos_dds_source_prefix.sh — validate a source-built OHOS DDS prefix.
#
# This helper is intentionally DDS-only. It validates the source-built prefix used
# by OHOS cross builds before IBMW links against it; it does not require ROS 2
# message packages or rclcpp and must not use host /opt/ros/humble as target
# evidence.
#
# Usage:
#   bash scripts/deps/check_ohos_dds_source_prefix.sh <prefix> [toolchain-file]
#
# Examples:
#   bash scripts/deps/check_ohos_dds_source_prefix.sh /tmp/ohos-dds-prefix
#   bash scripts/deps/check_ohos_dds_source_prefix.sh /tmp/ohos-dds-prefix cmake/toolchains/ohos-arm64-v8a.cmake

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  check_ohos_dds_source_prefix.sh <prefix> [toolchain-file]

Validates a source-built OHOS DDS dependency prefix by running a clean CMake
configure probe with the OHOS toolchain and the explicit prefix only.

Required target-discoverable packages:
  fastcdr
  fastdds or fastrtps
  tinyxml2
  foonathan_memory

The checker rejects host leakage in CMake probe artifacts, requires OHOS cross
compile metadata, ensures resolved package paths stay inside the prefix, and
best-effort verifies that discovered libraries are not x86 host objects.
EOF
}

fail() {
  echo "[ohos-dds-prefix] ERROR: $*" >&2
  exit 1
}

note() {
  echo "[ohos-dds-prefix] $*"
}

warn() {
  echo "[ohos-dds-prefix] warning: $*"
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage >&2
  exit 64
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TARGET_PREFIX="$1"
TOOLCHAIN_FILE="${2:-$REPO_ROOT/cmake/toolchains/ohos-arm64-v8a.cmake}"

extract_ohos_sdk_native_root() {
  local candidate=""

  if [[ -n "${OHOS_SDK_NATIVE_ROOT:-}" ]]; then
    candidate="$OHOS_SDK_NATIVE_ROOT"
  else
    candidate="$({ grep -E '^[[:space:]]*set\(OHOS_SDK_NATIVE_ROOT[[:space:]]+"[^"]+"' "$TOOLCHAIN_FILE" || true; } \
      | sed -E 's/^[[:space:]]*set\(OHOS_SDK_NATIVE_ROOT[[:space:]]+"([^"]+)".*$/\1/' \
      | head -n 1)"
  fi

  if [[ -n "$candidate" && -d "$candidate" ]]; then
    (cd "$candidate" && pwd)
  fi
}

append_pkgconfig_dir() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0
  OHOS_PKG_CONFIG_DIRS+=("$dir")
}

if [[ ! -d "$TARGET_PREFIX" ]]; then
  echo "[ohos-dds-prefix] ERROR: target prefix not found: $TARGET_PREFIX" >&2
  exit 64
fi

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
  echo "[ohos-dds-prefix] ERROR: OHOS toolchain file not found: $TOOLCHAIN_FILE" >&2
  exit 64
fi

TARGET_PREFIX="$(cd "$TARGET_PREFIX" && pwd)"
TOOLCHAIN_FILE="$(cd "$(dirname "$TOOLCHAIN_FILE")" && pwd)/$(basename "$TOOLCHAIN_FILE")"

OHOS_SDK_NATIVE_ROOT_RESOLVED="$(extract_ohos_sdk_native_root)"

case "$TARGET_PREFIX" in
  /opt/ros/humble|/opt/ros/humble/*)
    fail "host /opt/ros/humble must not be used as OHOS DDS source prefix"
    ;;
  /usr|/usr/*)
    fail "system host prefix must not be used as OHOS DDS source prefix: $TARGET_PREFIX"
    ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
  fail "cmake is required"
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ibmw_ohos_dds_prefix.XXXXXX")"
cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

PROBE_SRC="$BUILD_DIR/src"
PROBE_BUILD="$BUILD_DIR/build"
mkdir -p "$PROBE_SRC" "$PROBE_BUILD"

cat >"$PROBE_SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(ibmw_ohos_dds_source_prefix_probe LANGUAGES C CXX)

if(NOT DEFINED IBMW_DDS_SOURCE_PREFIX OR IBMW_DDS_SOURCE_PREFIX STREQUAL "")
  message(FATAL_ERROR "IBMW_DDS_SOURCE_PREFIX is required")
endif()

file(REAL_PATH "${IBMW_DDS_SOURCE_PREFIX}" _ibmw_prefix)

if(NOT CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "expected CMAKE_CROSSCOMPILING=TRUE for OHOS probe")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  message(FATAL_ERROR
    "expected CMAKE_SYSTEM_NAME=OHOS, got '${CMAKE_SYSTEM_NAME}'")
endif()

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
        if(path_item MATCHES "^/usr/lib/x86_64-linux-gnu(/|$)")
          message(FATAL_ERROR
            "${package_name} resolved to host x86_64 system path: ${path_item}")
        endif()
        if(path_item MATCHES "(^|[^A-Za-z0-9_])(x86_64|amd64)([^A-Za-z0-9_]|$)")
          message(FATAL_ERROR
            "${package_name} resolved to suspicious host architecture path: ${path_item}")
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
message(STATUS "IBMW_CMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}")
message(STATUS "IBMW_CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
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

declare -a OHOS_PKG_CONFIG_DIRS=()
for pkg_dir in \
  "$TARGET_PREFIX/lib/pkgconfig" \
  "$TARGET_PREFIX/lib64/pkgconfig" \
  "$TARGET_PREFIX/share/pkgconfig" \
  "$TARGET_PREFIX/lib/aarch64-linux-gnu/pkgconfig" \
  "$TARGET_PREFIX/lib/arm64-v8a/pkgconfig" \
  "$TARGET_PREFIX/lib/aarch64-linux-ohos/pkgconfig" \
  "$TARGET_PREFIX/lib64/aarch64-linux-ohos/pkgconfig"; do
  append_pkgconfig_dir "$pkg_dir"
done

OHOS_PKG_CONFIG_SYSROOT_DIR=""
if [[ -n "$OHOS_SDK_NATIVE_ROOT_RESOLVED" ]]; then
  for sysroot_dir in \
    "$OHOS_SDK_NATIVE_ROOT_RESOLVED/sysroot" \
    "$OHOS_SDK_NATIVE_ROOT_RESOLVED/sysroot/usr"; do
    for pkg_dir in \
      "$sysroot_dir/lib/pkgconfig" \
      "$sysroot_dir/lib64/pkgconfig" \
      "$sysroot_dir/share/pkgconfig" \
      "$sysroot_dir/usr/lib/pkgconfig" \
      "$sysroot_dir/usr/lib64/pkgconfig" \
      "$sysroot_dir/usr/share/pkgconfig" \
      "$sysroot_dir/usr/lib/aarch64-linux-ohos/pkgconfig" \
      "$sysroot_dir/usr/lib64/aarch64-linux-ohos/pkgconfig"; do
      append_pkgconfig_dir "$pkg_dir"
    done
  done

  if [[ -d "$OHOS_SDK_NATIVE_ROOT_RESOLVED/sysroot" ]]; then
    OHOS_PKG_CONFIG_SYSROOT_DIR="$OHOS_SDK_NATIVE_ROOT_RESOLVED/sysroot"
  fi
fi

declare -A _ibmw_seen_pkg_dirs=()
declare -a OHOS_PKG_CONFIG_DIRS_UNIQ=()
for pkg_dir in "${OHOS_PKG_CONFIG_DIRS[@]}"; do
  if [[ -z "${_ibmw_seen_pkg_dirs[$pkg_dir]:-}" ]]; then
    _ibmw_seen_pkg_dirs[$pkg_dir]=1
    OHOS_PKG_CONFIG_DIRS_UNIQ+=("$pkg_dir")
  fi
done
OHOS_PKG_CONFIG_LIBDIR="$(IFS=:; printf '%s' "${OHOS_PKG_CONFIG_DIRS_UNIQ[*]:-}")"

declare -a CMAKE_PROBE_ARGS=(
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
  -DCMAKE_PREFIX_PATH="$TARGET_PREFIX"
  -DCMAKE_FIND_ROOT_PATH="$TARGET_PREFIX"
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
  -DIBMW_DDS_SOURCE_PREFIX="$TARGET_PREFIX"
)

if [[ -n "$OHOS_SDK_NATIVE_ROOT_RESOLVED" ]]; then
  CMAKE_PROBE_ARGS+=(-DOHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT_RESOLVED")
fi

note "running clean CMake configure probe"
cmake -E env \
  PKG_CONFIG_PATH= \
  PKG_CONFIG_DIR= \
  PKG_CONFIG_LIBDIR="$OHOS_PKG_CONFIG_LIBDIR" \
  PKG_CONFIG_SYSROOT_DIR="$OHOS_PKG_CONFIG_SYSROOT_DIR" \
  cmake -S "$PROBE_SRC" -B "$PROBE_BUILD" "${CMAKE_PROBE_ARGS[@]}" \
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

if ! grep -q 'IBMW_CMAKE_CROSSCOMPILING=TRUE' "$BUILD_DIR/cmake_configure.log"; then
  fail "CMake probe did not report CMAKE_CROSSCOMPILING=TRUE"
fi
if ! grep -q 'IBMW_CMAKE_SYSTEM_NAME=OHOS' "$BUILD_DIR/cmake_configure.log"; then
  fail "CMake probe did not report CMAKE_SYSTEM_NAME=OHOS"
fi

LEAK_REPORT="$BUILD_DIR/host_leak_report.txt"
if grep -E '/opt/ros/humble|/usr/lib/x86_64-linux-gnu' "${PROBE_ARTIFACTS[@]}" >"$LEAK_REPORT" 2>/dev/null; then
  cat "$LEAK_REPORT" >&2
  fail "host leakage detected in CMake probe artifacts"
fi

declare -a RESOLVED_PACKAGE_DIRS=()
while IFS= read -r resolved_dir; do
  [[ -n "$resolved_dir" ]] || continue
  RESOLVED_PACKAGE_DIRS+=("$resolved_dir")
done < <(grep -Eo 'IBMW_(FASTCDR|FASTDDS|FASTRTPS|TINYXML2|FOONATHAN_MEMORY)_DIR=.*' "$BUILD_DIR/cmake_configure.log" | sed 's/^[^=]*=//')

if (( ${#RESOLVED_PACKAGE_DIRS[@]} == 0 )); then
  fail "failed to extract resolved package directories from CMake probe log"
fi

for resolved_dir in "${RESOLVED_PACKAGE_DIRS[@]}"; do
  if [[ "$resolved_dir" =~ (^|[^A-Za-z0-9_])(x86_64|amd64)([^A-Za-z0-9_]|$) ]]; then
    fail "resolved package path contains host architecture token: $resolved_dir"
  fi
  case "$resolved_dir" in
    "$TARGET_PREFIX"|"$TARGET_PREFIX"/*)
      ;;
    *)
      fail "resolved package path escaped prefix: $resolved_dir"
      ;;
  esac
done

declare -a SEARCH_DIRS=()
for lib_dir in \
  "$TARGET_PREFIX/lib" \
  "$TARGET_PREFIX/lib64" \
  "$TARGET_PREFIX/lib/aarch64-linux-gnu" \
  "$TARGET_PREFIX/lib/arm64-v8a" \
  "$TARGET_PREFIX/lib/aarch64-linux-ohos" \
  "$TARGET_PREFIX/lib64/aarch64-linux-ohos"; do
  if [[ -d "$lib_dir" ]]; then
    SEARCH_DIRS+=("$lib_dir")
  fi
done

if (( ${#SEARCH_DIRS[@]} == 0 )); then
  fail "no library directories found under prefix"
fi

find_first_lib() {
  local pattern
  for pattern in "$@"; do
    local dir
    for dir in "${SEARCH_DIRS[@]}"; do
      while IFS= read -r -d '' candidate; do
        printf '%s\n' "$candidate"
        return 0
      done < <(find "$dir" -maxdepth 1 -type f -name "$pattern" -print0)
    done
  done
  return 1
}

declare -a LIB_CANDIDATES=()
FASTCDR_LIB="$(find_first_lib 'libfastcdr.so*' 'libfastcdr.a')" || fail "missing fastcdr library candidate under prefix"
LIB_CANDIDATES+=("$FASTCDR_LIB")

if FASTDDS_LIB="$(find_first_lib 'libfastdds.so*' 'libfastdds.a')"; then
  LIB_CANDIDATES+=("$FASTDDS_LIB")
elif FASTRTPS_LIB="$(find_first_lib 'libfastrtps.so*' 'libfastrtps.a')"; then
  LIB_CANDIDATES+=("$FASTRTPS_LIB")
else
  fail "missing fastdds/fastrtps library candidate under prefix"
fi

TINYXML2_LIB="$(find_first_lib 'libtinyxml2.so*' 'libtinyxml2.a')" || fail "missing tinyxml2 library candidate under prefix"
LIB_CANDIDATES+=("$TINYXML2_LIB")

FOONATHAN_LIB="$(find_first_lib 'libfoonathan_memory*.so*' 'libfoonathan_memory*.a')" || fail "missing foonathan_memory library candidate under prefix"
LIB_CANDIDATES+=("$FOONATHAN_LIB")

if command -v file >/dev/null 2>&1; then
  for lib_path in "${LIB_CANDIDATES[@]}"; do
    file_output="$(file -L "$lib_path")"
    case "$file_output" in
      *"ARM aarch64"*|*"AArch64"*|*"ARM64"*|*"OHOS"*|*"current ar archive"*)
        ;;
      *"x86-64"*|*"x86_64"*|*"80386"*|*"Intel"*|*"AMD"*)
        fail "non-OHOS host library detected: $file_output"
        ;;
      *)
        warn "unable to classify library architecture with file: $file_output"
        ;;
    esac
  done
else
  warn "file command not available; skipping library architecture check"
fi

if command -v readelf >/dev/null 2>&1; then
  for lib_path in "${LIB_CANDIDATES[@]}"; do
    if [[ "$lib_path" == *.a ]]; then
      warn "skipping readelf check for archive: $lib_path"
      continue
    fi
    if readelf_header="$(readelf -h "$lib_path" 2>/dev/null)"; then
      if grep -Eq 'Machine:[[:space:]]*(Advanced Micro Devices X86-64|Intel 80386|AArch32)' <<<"$readelf_header"; then
        fail "readelf reports host or wrong-arch shared library: $lib_path"
      fi
      if grep -q 'Machine:[[:space:]]*AArch64' <<<"$readelf_header"; then
        :
      else
        warn "readelf could not confirm AArch64 machine for $lib_path"
      fi
    else
      warn "readelf could not parse library header: $lib_path"
    fi
  done
else
  warn "readelf command not available; skipping ELF header architecture check"
fi

note "PASS: source-built OHOS DDS prefix is target-discoverable and free of host leakage."
