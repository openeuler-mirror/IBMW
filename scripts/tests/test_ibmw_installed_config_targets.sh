#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Standalone regression test for installed ibmw config provider targets.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMPLATE="$REPO_ROOT/cmake/ibmw-config.cmake.in"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir -p "$SCRATCH/fakes/lib/cmake" "$SCRATCH/helpers" "$SCRATCH/tools"
for package in fmt stdexec fastcdr fastrtps fastdds; do
  package_dir="$SCRATCH/fakes/lib/cmake/$package"
  mkdir -p "$package_dir"
  cat >"$package_dir/${package}Config.cmake" <<EOF
set(${package}_FOUND TRUE)
if(NOT TARGET ${package})
  add_library(${package} INTERFACE IMPORTED)
endif()
EOF
done

cat >"$SCRATCH/render.cmake" <<'EOF'
set(PACKAGE_INIT [=[
set(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
macro(set_and_check name path)
  set(${name} "${path}")
endmacro()
macro(check_required_components name)
endmacro()
]=])
set(PACKAGE_IBMW_HELPERS_DIR "@HELPERS@")
set(PACKAGE_IBMW_DDS_IDL_RPC_GEN_TOOL_PATH "@TOOLS@")
set(IBMW_EXECUTOR_USE_STDEXEC ON)
set(IBMW_BUILD_RUNTIME OFF)
set(IBMW_BUILD_PROTOCOLS ON)
set(IBMW_BUILD_DDS_EXTENSION OFF)
set(IBMW_BUILD_ROS2_TYPES OFF)
set(IBMW_BUILD_WITH_ROS2 OFF)
set(IBMW_TARGET_ROS2_TIER "${TIER}")
configure_file("${INPUT}" "${OUTPUT}" @ONLY)
EOF
sed -i "s|@HELPERS@|$SCRATCH/helpers|g; s|@TOOLS@|$SCRATCH/tools|g" "$SCRATCH/render.cmake"

run_case() {
  local tier="$1"
  local expect_targets="$2"
  local case_dir="$SCRATCH/$tier"
  local install_dir="$case_dir/prefix/lib/cmake/ibmw"
  mkdir -p "$install_dir" "$case_dir/consumer"

  cmake -DINPUT="$TEMPLATE" -DOUTPUT="$install_dir/ibmw-config.cmake" \
    -DTIER="$tier" -P "$SCRATCH/render.cmake"

  cat >"$install_dir/ibmw-targets.cmake" <<EOF
if("$expect_targets" STREQUAL "yes")
  if(NOT TARGET ibmw_target_fastcdr OR NOT TARGET ibmw_target_fastrtps)
    message(FATAL_ERROR "rosidl config did not recreate canonical provider targets")
  endif()
else()
  if(TARGET ibmw_target_fastcdr OR TARGET ibmw_target_fastrtps)
    message(FATAL_ERROR "non-rosidl config created Route-B compatibility targets")
  endif()
endif()
add_library(ibmw::probe INTERFACE IMPORTED)
if("$expect_targets" STREQUAL "yes")
  set_property(TARGET ibmw::probe PROPERTY INTERFACE_LINK_LIBRARIES
    "ibmw_target_fastcdr;ibmw_target_fastrtps")
endif()
EOF

  cat >"$case_dir/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(ibmw_config_target_probe NONE)
find_package(ibmw REQUIRED CONFIG)
EOF

  env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
    cmake -S "$case_dir/consumer" -B "$case_dir/build" \
      -DCMAKE_PREFIX_PATH="$case_dir/prefix;$SCRATCH/fakes" >/dev/null
}

run_case rosidl yes
run_case native no

echo "PASS: rosidl installed config recreates canonical DDS provider targets; non-rosidl config does not."
