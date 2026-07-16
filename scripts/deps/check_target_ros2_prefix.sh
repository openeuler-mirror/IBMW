#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_target_ros2_prefix.sh - validate an explicit target ROS 2 prefix.
#
# This helper checks only the direct packages needed by a selected feature tier.
# It intentionally does NOT walk the full transitive ROS 2 closure. A package is
# considered present only when the prefix exposes concrete provenance metadata
# such as package.xml or an exported CMake package config; a bare directory named
# after the package is not enough. The fastdds feature also checks direct target
# library provenance for FastCDR and FastDDS/FastRTPS.
#
# Features:
#   fastdds  - fastcdr + fastdds/fastrtps direct package + library presence
#   types    - std_msgs + geometry_msgs, with sensor_msgs reported as optional
#   rosidl   - ament_cmake + rosidl_default_generators + builtin_interfaces
#   action   - ROS2 Action metadata needed by the private Fibonacci DDS gate
#   rclcpp   - explicitly deferred/unsupported here; exits non-zero on purpose
#
# Usage:
#   bash scripts/deps/check_target_ros2_prefix.sh <prefix-or-list> <feature> [feature...]
#
# Examples:
#   bash scripts/deps/check_target_ros2_prefix.sh /opt/ros/humble fastdds types
#   bash scripts/deps/check_target_ros2_prefix.sh /tmp/ros-prefix rosidl
#   bash scripts/deps/check_target_ros2_prefix.sh /tmp/overlay:/tmp/underlay types

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  check_target_ros2_prefix.sh <prefix-or-list> <feature> [feature...]

Features:
  fastdds  check direct FastDDS/FastCDR package and library presence
  types    check direct ROS message type packages
  rosidl   check direct rosidl generator packages
  action   check direct ROS2 Action package metadata
  rclcpp   explicitly deferred / unsupported in this lightweight validator
EOF
}

if [[ $# -lt 2 ]]; then
  usage >&2
  exit 64
fi

TARGET_PREFIX_SPEC="$1"
shift

declare -a TARGET_PREFIXES=()
if [[ -z "$TARGET_PREFIX_SPEC" || "$TARGET_PREFIX_SPEC" =~ (^|[:\;])($|[:\;]) ]]; then
  echo "[ros2-prefix] ERROR: empty entry in target prefix list: $TARGET_PREFIX_SPEC" >&2
  exit 64
fi

IFS=':;' read -r -a TARGET_PREFIXES <<< "$TARGET_PREFIX_SPEC"
if (( ${#TARGET_PREFIXES[@]} == 0 )); then
  echo "[ros2-prefix] ERROR: empty target prefix list" >&2
  exit 64
fi

for prefix in "${TARGET_PREFIXES[@]}"; do
  if [[ -z "$prefix" ]]; then
    echo "[ros2-prefix] ERROR: empty entry in target prefix list: $TARGET_PREFIX_SPEC" >&2
    exit 64
  fi
  if [[ ! -d "$prefix" ]]; then
    echo "[ros2-prefix] ERROR: target prefix not found: $prefix" >&2
    exit 64
  fi
done

declare -a FEATURES=("$@")
declare -a MISSING=()
declare -a OPTIONAL_MISSING=()

package_evidence() {
  local pkg="$1"
  local prefix
  for prefix in "${TARGET_PREFIXES[@]}"; do
    local candidates=()
    shopt -s nullglob
    candidates=(
      "$prefix/share/$pkg/package.xml"
      "$prefix/share/$pkg/cmake/"*Config.cmake
      "$prefix/share/$pkg/cmake/"*-config.cmake
      "$prefix/lib/cmake/$pkg/"*Config.cmake
      "$prefix/lib/cmake/$pkg/"*-config.cmake
    )
    shopt -u nullglob
    local candidate
    for candidate in "${candidates[@]}"; do
      if [[ -f "$candidate" ]]; then
        echo "${prefix}:${candidate#"$prefix/"}"
        return 0
      fi
    done
  done
  return 1
}

has_package() {
  package_evidence "$1" >/dev/null
}

report_package() {
  local feature="$1"
  local pkg="$2"
  local evidence
  if evidence="$(package_evidence "$pkg")"; then
    echo "[ros2-prefix] found ${feature}:${pkg} -> ${evidence}"
    return 0
  fi
  return 1
}

library_evidence() {
  local lib="$1"
  local prefix
  for prefix in "${TARGET_PREFIXES[@]}"; do
    local candidates=()
    shopt -s nullglob
    candidates=(
      "$prefix/lib/lib${lib}.so"
      "$prefix/lib/lib${lib}.so."*
      "$prefix/lib/lib${lib}.a"
      "$prefix/lib64/lib${lib}.so"
      "$prefix/lib64/lib${lib}.so."*
      "$prefix/lib64/lib${lib}.a"
    )
    shopt -u nullglob
    local candidate
    for candidate in "${candidates[@]}"; do
      if [[ -f "$candidate" ]]; then
        echo "${prefix}:${candidate#"$prefix/"}"
        return 0
      fi
    done
  done
  return 1
}

report_library() {
  local feature="$1"
  local lib="$2"
  local evidence
  if evidence="$(library_evidence "$lib")"; then
    echo "[ros2-prefix] found ${feature}:lib${lib} -> ${evidence}"
    return 0
  fi
  return 1
}

check_required_library() {
  local feature="$1"
  shift
  local lib
  for lib in "$@"; do
    if ! report_library "$feature" "$lib"; then
      MISSING+=("${feature}:lib${lib}")
    fi
  done
}

check_any_library() {
  local feature="$1"
  shift
  local lib found=0
  for lib in "$@"; do
    if report_library "$feature" "$lib"; then
      found=1
      break
    fi
  done
  if [[ $found -eq 0 ]]; then
    MISSING+=("${feature}:one-of-lib[$*]")
  fi
}

check_required() {
  local feature="$1"
  shift
  local pkg
  for pkg in "$@"; do
    if ! report_package "$feature" "$pkg"; then
      MISSING+=("${feature}:${pkg}")
    fi
  done
}

check_any() {
  local feature="$1"
  shift
  local pkg found=0
  for pkg in "$@"; do
    if report_package "$feature" "$pkg"; then
      found=1
      break
    fi
  done
  if [[ $found -eq 0 ]]; then
    MISSING+=("${feature}:one-of[$*]")
  fi
}

for feature in "${FEATURES[@]}"; do
  case "$feature" in
    fastdds)
      echo "[ros2-prefix] feature=fastdds -> checking direct packages and libraries only"
      check_required "$feature" fastcdr
      check_any "$feature" fastdds fastrtps
      check_required_library "$feature" fastcdr
      check_any_library "$feature" fastdds fastrtps
      ;;
    types)
      echo "[ros2-prefix] feature=types -> checking direct message packages only"
      check_required "$feature" std_msgs geometry_msgs
      if has_package sensor_msgs; then
        echo "[ros2-prefix] note: sensor_msgs present (optional for types)"
      else
        OPTIONAL_MISSING+=("${feature}:sensor_msgs")
      fi
      ;;
    rosidl)
      echo "[ros2-prefix] feature=rosidl -> checking direct generator packages only"
      check_required "$feature" ament_cmake rosidl_default_generators builtin_interfaces
      ;;
    action)
      echo "[ros2-prefix] feature=action -> checking direct ROS2 Action packages only"
      check_required "$feature" action_msgs example_interfaces unique_identifier_msgs builtin_interfaces
      ;;
    rclcpp)
      echo "[ros2-prefix] ERROR: feature=rclcpp is deferred / unsupported in this lightweight checker" >&2
      exit 3
      ;;
    *)
      echo "[ros2-prefix] ERROR: unknown feature: $feature" >&2
      usage >&2
      exit 64
      ;;
  esac
done

if (( ${#MISSING[@]} > 0 )); then
  echo "[ros2-prefix] ERROR: missing required direct package/library evidence:" >&2
  for item in "${MISSING[@]}"; do
    echo "  - $item" >&2
  done
  exit 1
fi

if (( ${#OPTIONAL_MISSING[@]} > 0 )); then
  echo "[ros2-prefix] note: optional package sensor_msgs is absent (not a failure)."
fi

echo "[ros2-prefix] PASS: explicit target prefix contains the requested direct package/library evidence only."
