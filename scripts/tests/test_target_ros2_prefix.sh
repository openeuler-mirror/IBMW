#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Synthetic test harness for scripts/deps/check_target_ros2_prefix.sh.
#
# The harness builds fake target prefixes in a scratch directory and verifies
# the current AArch64 ROS2 deps-prefix preflight contract:
#   1) a complete direct package prefix passes;
#   2) optional sensor_msgs absence is non-fatal;
#   3) a prefix with bare package directories but no metadata is rejected;
#   4) an incomplete prefix reports the known missing ROS2 message/rosidl set;
#   5) rclcpp remains explicitly unsupported.
#   6) leading, middle, and trailing empty prefix-list entries are rejected.
#   7) FastDDS package metadata alone is rejected without direct library evidence.
#   8) private Action gate metadata passes only when the direct packages exist.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECKER="$REPO_ROOT/scripts/deps/check_target_ros2_prefix.sh"

if [[ ! -x "$CHECKER" ]]; then
  echo "FATAL: checker not found or not executable: $CHECKER" >&2
  exit 1
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  if [[ $# -gt 1 ]]; then
    echo "-- command output --" >&2
    echo "$2" >&2
  fi
  exit 1
}

make_pkg_xml() {
  local prefix="$1"
  local pkg="$2"
  mkdir -p "$prefix/share/$pkg"
  cat > "$prefix/share/$pkg/package.xml" <<EOF
<package format="3"><name>$pkg</name><version>0.0.0</version><description>synthetic</description><maintainer email="ibmw@example.invalid">IBMW</maintainer><license>synthetic</license></package>
EOF
}

make_cmake_pkg() {
  local prefix="$1"
  local pkg="$2"
  mkdir -p "$prefix/lib/cmake/$pkg"
  printf '# synthetic %s config\n' "$pkg" > "$prefix/lib/cmake/$pkg/${pkg}Config.cmake"
}

make_lib() {
  local prefix="$1"
  local lib="$2"
  mkdir -p "$prefix/lib"
  printf 'synthetic %s target library\n' "$lib" > "$prefix/lib/lib${lib}.so"
}

complete_prefix="$SCRATCH/complete"
make_cmake_pkg "$complete_prefix" fastcdr
make_cmake_pkg "$complete_prefix" fastdds
make_lib "$complete_prefix" fastcdr
make_lib "$complete_prefix" fastdds
make_pkg_xml "$complete_prefix" std_msgs
make_pkg_xml "$complete_prefix" geometry_msgs
make_pkg_xml "$complete_prefix" ament_cmake
make_pkg_xml "$complete_prefix" rosidl_default_generators
make_pkg_xml "$complete_prefix" builtin_interfaces
make_pkg_xml "$complete_prefix" action_msgs
make_pkg_xml "$complete_prefix" example_interfaces
make_pkg_xml "$complete_prefix" unique_identifier_msgs

OUT="$(bash "$CHECKER" "$complete_prefix" fastdds types rosidl action 2>&1)" \
  || fail "complete fake prefix did not pass" "$OUT"
echo "$OUT" | grep -F "PASS: explicit target prefix contains the requested direct package/library evidence only." >/dev/null \
  || fail "complete prefix output did not contain PASS line" "$OUT"
echo "$OUT" | grep -F "optional package sensor_msgs is absent (not a failure)" >/dev/null \
  || fail "optional sensor_msgs absence was not reported as non-fatal" "$OUT"

fake_positive_prefix="$SCRATCH/fake-positive"
for pkg in fastcdr fastdds std_msgs geometry_msgs ament_cmake rosidl_default_generators builtin_interfaces action_msgs example_interfaces unique_identifier_msgs; do
  mkdir -p "$fake_positive_prefix/share/$pkg"
done
OUT="$(bash "$CHECKER" "$fake_positive_prefix" fastdds types rosidl action 2>&1 || true)"
echo "$OUT" | grep -F "ERROR: missing required direct package/library evidence:" >/dev/null \
  || fail "bare directories without package.xml/CMake config were accepted" "$OUT"
echo "$OUT" | grep -F "fastdds:fastcdr" >/dev/null \
  || fail "fake-positive prefix did not report missing fastcdr metadata" "$OUT"

metadata_only_prefix="$SCRATCH/metadata-only"
make_cmake_pkg "$metadata_only_prefix" fastcdr
make_cmake_pkg "$metadata_only_prefix" fastrtps
OUT="$(bash "$CHECKER" "$metadata_only_prefix" fastdds 2>&1 || true)"
for expected in "fastdds:libfastcdr" "fastdds:one-of-lib[fastdds fastrtps]"; do
  echo "$OUT" | grep -F "$expected" >/dev/null \
    || fail "metadata-only FastDDS prefix did not report $expected" "$OUT"
done

fastrtps_prefix="$SCRATCH/fastrtps-alt"
make_cmake_pkg "$fastrtps_prefix" fastcdr
make_cmake_pkg "$fastrtps_prefix" fastrtps
make_lib "$fastrtps_prefix" fastcdr
make_lib "$fastrtps_prefix" fastrtps
OUT="$(bash "$CHECKER" "$fastrtps_prefix" fastdds 2>&1)" \
  || fail "fastrtps alternative prefix did not satisfy fastdds feature" "$OUT"
echo "$OUT" | grep -F "found fastdds:fastrtps" >/dev/null \
  || fail "fastrtps package alternative was not reported" "$OUT"
echo "$OUT" | grep -F "found fastdds:libfastrtps" >/dev/null \
  || fail "fastrtps library alternative was not reported" "$OUT"

incomplete_prefix="$SCRATCH/incomplete"
make_cmake_pkg "$incomplete_prefix" fastcdr
make_lib "$incomplete_prefix" fastcdr
OUT="$(bash "$CHECKER" "$incomplete_prefix" fastdds types rosidl action 2>&1 || true)"
for expected in \
  "fastdds:one-of[fastdds fastrtps]" \
  "types:std_msgs" \
  "types:geometry_msgs" \
  "rosidl:ament_cmake" \
  "rosidl:rosidl_default_generators" \
  "rosidl:builtin_interfaces" \
  "action:action_msgs" \
  "action:example_interfaces" \
  "action:unique_identifier_msgs" \
  "action:builtin_interfaces"; do
  echo "$OUT" | grep -F "$expected" >/dev/null \
    || fail "incomplete prefix did not report $expected" "$OUT"
done

for bad_spec in \
  ":$complete_prefix" \
  "$complete_prefix::$complete_prefix" \
  "$complete_prefix:;$complete_prefix" \
  "$complete_prefix;:$complete_prefix" \
  "$complete_prefix:" \
  ";$complete_prefix" \
  "$complete_prefix;;$complete_prefix" \
  "$complete_prefix;"; do
  OUT="$(bash "$CHECKER" "$bad_spec" fastdds 2>&1 || true)"
  echo "$OUT" | grep -F "ERROR: empty entry in target prefix list:" >/dev/null \
    || fail "empty prefix-list entry was accepted for '$bad_spec'" "$OUT"
done

set +e
OUT="$(bash "$CHECKER" "$complete_prefix" rclcpp 2>&1)"
status=$?
set -e
if [[ $status -eq 0 ]]; then
  fail "rclcpp was accepted but must remain unsupported" "$OUT"
fi
if [[ $status -ne 3 ]]; then
  fail "rclcpp returned $status, expected 3" "$OUT"
fi
echo "$OUT" | grep -F "feature=rclcpp is deferred / unsupported" >/dev/null \
  || fail "rclcpp rejection did not explain unsupported status" "$OUT"

echo "PASS: target ROS2 prefix preflight accepts real metadata/libs, rejects fake/incomplete/empty prefixes, covers Action metadata, keeps rclcpp unsupported, and treats sensor_msgs as optional."
