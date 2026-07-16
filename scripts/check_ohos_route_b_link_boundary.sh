#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_ohos_route_b_link_boundary.sh — scan a completed OHOS Route B build
# directory for host leakage and forbidden ROS2 runtime link dependencies.

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/check_ohos_route_b_link_boundary.sh [build-dir] [audit-json]

Validates an existing completed OHOS Route B CMake build directory. The helper
does not configure, build, or run board/runtime validation.

If build-dir is omitted, the helper scans:
  <repo>/build-ohos-arm64-dds-source-deps

If audit-json is omitted, the target artifact audit report is written to:
  <build-dir>/logs/route_b_target_artifact_audit.json
EOF
}

fail() {
  echo "[ohos-route-b-link] FAIL: $*" >&2
  exit 1
}

error_exit() {
  echo "[ohos-route-b-link] FAIL: $*" >&2
  exit 2
}

note() {
  echo "[ohos-route-b-link] $*"
}

pass() {
  echo "[ohos-route-b-link] PASS: $*"
}

if [[ $# -gt 2 ]]; then
  usage >&2
  exit 64
fi

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_BUILD_DIR="$REPO_ROOT/build-ohos-arm64-dds-source-deps"
BUILD_DIR="${1:-$DEFAULT_BUILD_DIR}"
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
AUDIT_JSON="${2:-$BUILD_DIR/logs/route_b_target_artifact_audit.json}"

case "$BUILD_DIR" in
  ""|"."|".."|"/"|"~"|"-"*)
    error_exit "refusing unsafe build dir: $BUILD_DIR"
    ;;
esac

if [[ ! -d "$BUILD_DIR" ]]; then
  fail "build dir not found: $BUILD_DIR"
fi

if ! command -v readelf >/dev/null 2>&1; then
  fail "readelf is required to audit target ELF DT_NEEDED entries"
fi

if [[ ! -f "$CACHE_FILE" ]]; then
  fail "CMakeCache.txt not found in build dir: $BUILD_DIR"
fi

require_cache_value() {
  local cache_name="$1"
  local expected_value="$2"

  if ! grep -Eq "^${cache_name}:(BOOL|STRING|INTERNAL)=${expected_value}$" "$CACHE_FILE"; then
    fail "expected ${cache_name} to be ${expected_value} in $CACHE_FILE"
  fi
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

is_generated_rosidl_fastrtps_link() {
  local artifact="$1"
  [[ "$artifact" == *__rosidl_typesupport_fastrtps_*.dir/link.txt ]]
}

is_generated_rosidl_pyext_link() {
  local artifact="$1"
  [[ "$artifact" == *__pyext.dir/link.txt ]]
}

contains_rmw_carveout_token() {
  local line="$1"
  [[ "$line" == *librmw.so* || "$line" == *rmw::rmw* ]]
}

contains_forbidden_runtime_token() {
  local line="$1"

  if [[ "$line" == *rmw::rmw* ||
        "$line" == *-lrmw* ||
        "$line" == *librmw.so* ||
        "$line" == *librcl.so* ||
        "$line" == *librclcpp.so* ||
        "$line" == *librcl_action.so* ||
        "$line" == *librclcpp_action.so* ||
        "$line" == *rmw_fastrtps* ||
        "$line" == *rmw_cyclonedds* ]]; then
    return 0
  fi

  if [[ "$line" =~ (^|[^[:alnum:]_])rclcpp_action([^[:alnum:]_]|$) ||
        "$line" =~ (^|[^[:alnum:]_])rcl_action([^[:alnum:]_]|$) ||
        "$line" =~ (^|[^[:alnum:]_])rclcpp([^[:alnum:]_]|$) ]]; then
    return 0
  fi

  return 1
}

check_forbidden_line_match() {
  local artifact="$1"
  local line="$2"
  local label="$3"
  local strict="$4"

  if [[ "$strict" != "yes" ]] && is_allowed_host_tool_line "$line"; then
    return 0
  fi

  fail "$label in $artifact: $line"
}

require_cache_value 'IBMW_BUILD_WITH_ROS2' 'OFF'
require_cache_value 'IBMW_BUILD_ROS2_TYPES' 'ON'
require_cache_value 'IBMW_BUILD_ROS2_EXTENSION' 'OFF'
require_cache_value 'IBMW_TARGET_ROS2_TIER' 'rosidl'

declare -a LINK_ARTIFACTS=()
declare -a NON_LINK_ARTIFACTS=("$CACHE_FILE")

for artifact in \
  "$BUILD_DIR/build.ninja" \
  "$BUILD_DIR/compile_commands.json" \
  "$BUILD_DIR/CMakeFiles/CMakeOutput.log" \
  "$BUILD_DIR/CMakeFiles/CMakeError.log"; do
  if [[ -f "$artifact" ]]; then
    NON_LINK_ARTIFACTS+=("$artifact")
  fi
done

while IFS= read -r -d '' artifact; do
  NON_LINK_ARTIFACTS+=("$artifact")
done < <(find "$BUILD_DIR" -maxdepth 1 -type f -name '*.log' -print0 | sort -z)

while IFS= read -r -d '' artifact; do
  LINK_ARTIFACTS+=("$artifact")
done < <(find "$BUILD_DIR" -type f -path '*/CMakeFiles/*/link.txt' -print0 2>/dev/null | sort -z)

note "build: $BUILD_DIR"
note "scan: ${#NON_LINK_ARTIFACTS[@]} non-link artifacts, ${#LINK_ARTIFACTS[@]} link files"

for artifact in "${NON_LINK_ARTIFACTS[@]}"; do
  [[ -f "$artifact" ]] || continue
  while IFS= read -r line; do
    check_forbidden_line_match "$artifact" "$line" 'forbidden host ROS path' 'no'
  done < <(grep -Fn '/opt/ros/humble' "$artifact" || true)
  while IFS= read -r line; do
    check_forbidden_line_match "$artifact" "$line" 'forbidden host x86_64 library path' 'no'
  done < <(grep -Fn '/usr/lib/x86_64-linux-gnu' "$artifact" || true)
  while IFS= read -r line; do
    check_forbidden_line_match "$artifact" "$line" 'forbidden host architecture marker' 'no'
  done < <(grep -Ein 'x86_64|amd64' "$artifact" || true)
done

generated_rmw_hits=0
generated_pyext_hits=0

for artifact in "${LINK_ARTIFACTS[@]}"; do
  [[ -f "$artifact" ]] || continue
  if is_generated_rosidl_pyext_link "$artifact"; then
    generated_pyext_hits=$((generated_pyext_hits + 1))
    continue
  fi
  while IFS= read -r line; do
    check_forbidden_line_match "$artifact" "$line" 'forbidden host ROS path in link file' 'yes'
  done < <(grep -Fn '/opt/ros/humble' "$artifact" || true)
  while IFS= read -r line; do
    check_forbidden_line_match "$artifact" "$line" 'forbidden host x86_64 library path in link file' 'yes'
  done < <(grep -Fn '/usr/lib/x86_64-linux-gnu' "$artifact" || true)
  while IFS= read -r line; do
    check_forbidden_line_match "$artifact" "$line" 'forbidden host architecture marker in link file' 'yes'
  done < <(grep -Ein 'x86_64|amd64' "$artifact" || true)

  while IFS= read -r line; do
    if is_generated_rosidl_fastrtps_link "$artifact" && contains_rmw_carveout_token "$line"; then
      generated_rmw_hits=$((generated_rmw_hits + 1))
      continue
    fi
    if contains_forbidden_runtime_token "$line"; then
      fail "forbidden ROS2 runtime link token in $artifact: $line"
    fi
  done < "$artifact"
done

if [[ $generated_rmw_hits -gt 0 ]]; then
  note "generated ROSIDL FastRTPS librmw/rmw::rmw link hits are isolated and non-fatal: $generated_rmw_hits"
fi

if [[ $generated_pyext_hits -gt 0 ]]; then
  while IFS= read -r -d '' pyext_artifact; do
    fail "generated ROSIDL pyext artifact was built despite metadata-only carve-out: $pyext_artifact"
  done < <(find "$BUILD_DIR" -type f -path '*/rosidl_generator_py/*' -name '*cpython*.so' -print0 2>/dev/null | sort -z)
  note "generated ROSIDL pyext link metadata is isolated and non-fatal: $generated_pyext_hits"
fi

note "target artifact audit: scanning ELF files for host-prefix leaks and non-AArch64 machine type"
mkdir -p "$(dirname "$AUDIT_JSON")"
PYTHONPATH="$REPO_ROOT/src/tools/ibmw_gen${PYTHONPATH:+:$PYTHONPATH}" \
  python3 - "$BUILD_DIR" "$AUDIT_JSON" <<'PY'
import json
import os
import sys

from ros2_interface_import import audit_target_artifacts

build_dir = sys.argv[1]
audit_json = sys.argv[2]
artifacts = []
for root, dirs, files in os.walk(build_dir):
    dirs[:] = [d for d in dirs if d not in (".git", "CMakeFiles")]
    for name in files:
        path = os.path.join(root, name)
        try:
            with open(path, "rb") as f:
                if f.read(4) == b"\x7fELF":
                    artifacts.append(path)
        except OSError:
            continue

report = audit_target_artifacts(
    sorted(artifacts),
    forbidden_prefixes=["/opt/ros/humble", "/usr/lib/x86_64-linux-gnu"],
    expected_arch="aarch64",
)
with open(audit_json, "w", encoding="utf-8") as f:
    json.dump(report, f, sort_keys=True, indent=2)
    f.write("\n")
if not artifacts:
    print("[ohos-route-b-link] FAIL: no target ELF artifacts found for audit", file=sys.stderr)
    sys.exit(1)
if report["status"] == "fail":
    for reason in report["reasons"]:
        print(f"[ohos-route-b-link] FAIL: {reason}", file=sys.stderr)
    sys.exit(1)
print(f"[ohos-route-b-link] target artifact audit status: {report['status']} ({len(artifacts)} ELF artifacts)")
print(f"[ohos-route-b-link] target artifact audit report: {audit_json}")
PY

while IFS= read -r -d '' elf_artifact; do
  while IFS= read -r needed_line; do
    fail "absolute DT_NEEDED entry in target ELF $elf_artifact: $needed_line"
  done < <(readelf -d "$elf_artifact" 2>/dev/null | grep -E '\(NEEDED\).*Shared library: \[/' || true)
done < <(find "$BUILD_DIR" -type f -print0 2>/dev/null | sort -z)

pass 'Route B compile/link boundary validated.'
