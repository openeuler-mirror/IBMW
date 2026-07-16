#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Synthetic test harness for scripts/check_ohos_route_b_link_boundary.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECKER="$REPO_ROOT/scripts/check_ohos_route_b_link_boundary.sh"

if [[ ! -f "$CHECKER" ]]; then
  echo "FATAL: checker not found: $CHECKER" >&2
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

make_cache() {
  local build_dir="$1"
  local with_ros2="${2:-OFF}"

  mkdir -p "$build_dir/CMakeFiles/clean_target.dir"
  cat > "$build_dir/CMakeCache.txt" <<EOF
IBMW_BUILD_WITH_ROS2:BOOL=$with_ros2
IBMW_BUILD_ROS2_TYPES:BOOL=ON
IBMW_BUILD_ROS2_EXTENSION:BOOL=OFF
IBMW_TARGET_ROS2_TIER:STRING=rosidl
EOF
}

make_clean_build() {
  local build_dir="$1"

  make_cache "$build_dir"
  cat > "$build_dir/build.ninja" <<'EOF'
# synthetic ninja metadata
build clean_target: CXX_LINKER__clean_target
EOF
  cat > "$build_dir/compile_commands.json" <<'EOF'
[
  {"directory":"/tmp/synthetic","command":"/ohos/toolchain/clang++ -target aarch64-linux-ohos -c clean.cc","file":"clean.cc"}
]
EOF
  cat > "$build_dir/CMakeFiles/clean_target.dir/link.txt" <<'EOF'
/ohos/toolchain/clang++ clean.cc.o -L/ohos/arm64/lib -libmw_core -lfastcdr -lfastdds
EOF
}

make_fake_elf() {
  local path="$1"
  local arch="$2"
  local payload="${3:-}"

  python3 - "$path" "$arch" "$payload" <<'PY'
import sys

path, arch, payload = sys.argv[1:]
machine = {
    "x86": 3,
    "arm": 40,
    "x86_64": 62,
    "aarch64": 183,
}[arch]
data = bytearray(b"\x7fELF")
data.extend(b"\x02\x01\x01")
data.extend(b"\x00" * (18 - len(data)))
data.extend(machine.to_bytes(2, "little"))
data.extend(b"\x00" * 44)
data.extend(payload.encode("utf-8"))
with open(path, "wb") as f:
    f.write(data)
PY
}

run_expect_pass() {
  local label="$1"
  local build_dir="$2"
  shift 2
  local out

  out="$(bash "$CHECKER" "$build_dir" "$@" 2>&1)" \
    || fail "$label unexpectedly failed" "$out"
  echo "$out"
}

run_expect_fail() {
  local label="$1"
  local build_dir="$2"
  local out status

  set +e
  out="$(bash "$CHECKER" "$build_dir" 2>&1)"
  status=$?
  set -e
  if [[ $status -eq 0 ]]; then
    fail "$label unexpectedly passed" "$out"
  fi
  echo "$out"
}

# 1) clean build passes.
clean_build="$SCRATCH/clean-build"
make_clean_build "$clean_build"
make_fake_elf "$clean_build/libclean.so" aarch64
OUT="$(run_expect_pass 'clean build' "$clean_build")"
echo "$OUT" | grep -F 'PASS: Route B compile/link boundary validated.' >/dev/null \
  || fail 'clean build did not report PASS' "$OUT"

# 2) generated ROSIDL FastRTPS librmw.so is non-fatal and noted.
fastrtps_build="$SCRATCH/generated-fastrtps"
make_clean_build "$fastrtps_build"
make_fake_elf "$fastrtps_build/libgenerated.so" aarch64
mkdir -p "$fastrtps_build/CMakeFiles/ibmw_msgs__rosidl_typesupport_fastrtps_c.dir"
cat > "$fastrtps_build/CMakeFiles/ibmw_msgs__rosidl_typesupport_fastrtps_c.dir/link.txt" <<'EOF'
/ohos/toolchain/clang++ generated.cc.o /target/ros2/lib/librmw.so -lrosidl_typesupport_fastrtps_c
EOF
OUT="$(run_expect_pass 'generated FastRTPS librmw carve-out' "$fastrtps_build")"
echo "$OUT" | grep -F 'generated ROSIDL FastRTPS librmw/rmw::rmw link hits are isolated and non-fatal' >/dev/null \
  || fail 'generated FastRTPS carve-out was not noted' "$OUT"

# 3) host /opt/ros/humble fails.
host_ros_build="$SCRATCH/host-ros"
make_clean_build "$host_ros_build"
printf '/opt/ros/humble/lib/libfastcdr.so\n' >> "$host_ros_build/CMakeFiles/clean_target.dir/link.txt"
OUT="$(run_expect_fail 'host ROS path' "$host_ros_build")"
echo "$OUT" | grep -F '/opt/ros/humble' >/dev/null \
  || fail 'host ROS failure did not mention /opt/ros/humble' "$OUT"

# 4) host /usr/lib/x86_64-linux-gnu in link.txt fails.
host_lib_build="$SCRATCH/host-lib"
make_clean_build "$host_lib_build"
printf '/usr/lib/x86_64-linux-gnu/libstdc++.so\n' >> "$host_lib_build/CMakeFiles/clean_target.dir/link.txt"
OUT="$(run_expect_fail 'host x86_64 library path' "$host_lib_build")"
echo "$OUT" | grep -F '/usr/lib/x86_64-linux-gnu' >/dev/null \
  || fail 'host library failure did not mention /usr/lib/x86_64-linux-gnu' "$OUT"

# 5) x86_64 in link.txt fails.
arch_build="$SCRATCH/arch-leak"
make_clean_build "$arch_build"
printf 'toolchain-triplet=x86_64-linux-gnu\n' >> "$arch_build/CMakeFiles/clean_target.dir/link.txt"
OUT="$(run_expect_fail 'x86_64 architecture marker' "$arch_build")"
echo "$OUT" | grep -F 'x86_64' >/dev/null \
  || fail 'architecture failure did not mention x86_64' "$OUT"

# 6) librmw.so in ibmw_dds_extension link.txt fails.
dds_extension_build="$SCRATCH/dds-extension-rmw"
make_clean_build "$dds_extension_build"
mkdir -p "$dds_extension_build/CMakeFiles/ibmw_dds_extension.dir"
cat > "$dds_extension_build/CMakeFiles/ibmw_dds_extension.dir/link.txt" <<'EOF'
/ohos/toolchain/clang++ dds_extension.cc.o /target/ros2/lib/librmw.so -libmw_core
EOF
OUT="$(run_expect_fail 'dds extension librmw link' "$dds_extension_build")"
echo "$OUT" | grep -F 'librmw.so' >/dev/null \
  || fail 'dds extension runtime failure did not mention librmw.so' "$OUT"

# 7) rclcpp::rclcpp in runtime link fails.
rclcpp_build="$SCRATCH/rclcpp-runtime"
make_clean_build "$rclcpp_build"
printf 'rclcpp::rclcpp\n' >> "$rclcpp_build/CMakeFiles/clean_target.dir/link.txt"
OUT="$(run_expect_fail 'rclcpp runtime link' "$rclcpp_build")"
echo "$OUT" | grep -F 'rclcpp::rclcpp' >/dev/null \
  || fail 'rclcpp runtime failure did not mention rclcpp::rclcpp' "$OUT"

# 8) missing CMakeCache.txt exits nonzero with clear message.
missing_cache_build="$SCRATCH/missing-cache"
mkdir -p "$missing_cache_build"
OUT="$(run_expect_fail 'missing CMakeCache.txt' "$missing_cache_build")"
echo "$OUT" | grep -F 'CMakeCache.txt not found' >/dev/null \
  || fail 'missing cache failure was not clear' "$OUT"

# 9) bad cache mode such as IBMW_BUILD_WITH_ROS2=ON fails.
bad_cache_build="$SCRATCH/bad-cache"
make_clean_build "$bad_cache_build"
make_cache "$bad_cache_build" ON
OUT="$(run_expect_fail 'bad cache mode' "$bad_cache_build")"
echo "$OUT" | grep -F 'expected IBMW_BUILD_WITH_ROS2 to be OFF' >/dev/null \
  || fail 'bad cache failure did not explain expected mode' "$OUT"

# 10) AArch64 ELF target artifacts pass the Python artifact audit.
elf_ok_build="$SCRATCH/elf-ok"
make_clean_build "$elf_ok_build"
make_fake_elf "$elf_ok_build/libclean.so" aarch64
OUT="$(run_expect_pass 'aarch64 ELF artifact' "$elf_ok_build")"
echo "$OUT" | grep -F 'target artifact audit status: pass (1 ELF artifacts)' >/dev/null \
  || fail 'aarch64 ELF artifact audit was not reported as pass' "$OUT"
audit_json="$elf_ok_build/logs/route_b_target_artifact_audit.json"
[[ -f "$audit_json" ]] || fail 'route B target artifact audit JSON was not written' "$OUT"
python3 - "$audit_json" "$elf_ok_build/libclean.so" <<'PY'
import json
import pathlib
import sys

report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
artifact = str(pathlib.Path(sys.argv[2]))
if report["status"] != "pass":
    raise SystemExit(f"expected pass audit, got {report['status']}: {report['reasons']}")
if report["expected_arch"] != "aarch64":
    raise SystemExit(f"expected aarch64 audit, got {report['expected_arch']}")
if not report["artifacts"]:
    raise SystemExit("expected at least one audited ELF artifact")
if report["artifacts"][0]["path"] != artifact:
    raise SystemExit(f"unexpected artifact path: {report['artifacts']}")
if report["artifacts"][0]["arch"] != "aarch64":
    raise SystemExit(f"unexpected artifact arch: {report['artifacts']}")
PY

# 10b) a custom audit-json output path is supported for external evidence capture.
custom_audit_json="$SCRATCH/custom/route_b_target_artifact_audit.json"
OUT="$(run_expect_pass 'custom audit json path' "$elf_ok_build" "$custom_audit_json")"
[[ -f "$custom_audit_json" ]] || fail 'custom route B target artifact audit JSON was not written' "$OUT"
echo "$OUT" | grep -F "target artifact audit report: $custom_audit_json" >/dev/null \
  || fail 'custom audit JSON path was not reported' "$OUT"

# 11) wrong-arch ELF target artifacts fail the Python artifact audit.
elf_bad_build="$SCRATCH/elf-bad"
make_clean_build "$elf_bad_build"
make_fake_elf "$elf_bad_build/libbad.so" x86_64
OUT="$(run_expect_fail 'x86_64 ELF artifact' "$elf_bad_build")"
echo "$OUT" | grep -F 'arch x86_64 != expected aarch64' >/dev/null \
  || fail 'wrong-arch ELF artifact failure did not mention architecture mismatch' "$OUT"

# 12) generated rosidl_generator_py pyext link metadata is dormant and non-fatal.
pyext_metadata_build="$SCRATCH/pyext-metadata"
make_clean_build "$pyext_metadata_build"
make_fake_elf "$pyext_metadata_build/libclean.so" aarch64
mkdir -p "$pyext_metadata_build/CMakeFiles/ibmw_msgs__rosidl_typesupport_c__pyext.dir"
cat > "$pyext_metadata_build/CMakeFiles/ibmw_msgs__rosidl_typesupport_c__pyext.dir/link.txt" <<'EOF'
/ohos/toolchain/clang++ -shared -o rosidl_generator_py/ibmw_msgs/ibmw_msgs_s__rosidl_typesupport_c.cpython-313-x86_64-linux-gnu.so /host/python/lib/libpython3.13.so /target/ros2/lib/librmw.so
EOF
OUT="$(run_expect_pass 'dormant pyext metadata' "$pyext_metadata_build")"
echo "$OUT" | grep -F 'generated ROSIDL pyext link metadata is isolated and non-fatal: 1' >/dev/null \
  || fail 'dormant pyext metadata carve-out was not reported' "$OUT"

# 13) the pyext carve-out is narrow: a built cpython pyext artifact still fails.
pyext_built_build="$SCRATCH/pyext-built"
make_clean_build "$pyext_built_build"
make_fake_elf "$pyext_built_build/libclean.so" aarch64
mkdir -p "$pyext_built_build/CMakeFiles/ibmw_msgs__rosidl_typesupport_c__pyext.dir"
cat > "$pyext_built_build/CMakeFiles/ibmw_msgs__rosidl_typesupport_c__pyext.dir/link.txt" <<'EOF'
/ohos/toolchain/clang++ -shared -o rosidl_generator_py/ibmw_msgs/ibmw_msgs_s__rosidl_typesupport_c.cpython-313-x86_64-linux-gnu.so /host/python/lib/libpython3.13.so /target/ros2/lib/librmw.so
EOF
mkdir -p "$pyext_built_build/rosidl_generator_py/ibmw_msgs"
make_fake_elf "$pyext_built_build/rosidl_generator_py/ibmw_msgs/ibmw_msgs_s__rosidl_typesupport_c.cpython-313-x86_64-linux-gnu.so" x86_64
OUT="$(run_expect_fail 'built pyext artifact' "$pyext_built_build")"
echo "$OUT" | grep -F 'generated ROSIDL pyext artifact was built despite metadata-only carve-out' >/dev/null \
  || fail 'built pyext artifact failure did not mention metadata-only carve-out' "$OUT"

echo 'PASS: OHOS Route B link-boundary checker accepts clean/generated FastRTPS cases and rejects host/runtime/cache violations.'
