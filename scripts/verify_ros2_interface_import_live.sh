#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Reproducible live verification entrypoint for ROS2 interface import evidence.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERIFY_LANES_WAS_SET="${VERIFY_LANES+x}"
VERIFY_LANES="${VERIFY_LANES:-msg imported_srv reference_srv action}"
ACTION_RUN_CASES="${ACTION_RUN_CASES:-fibonacci gripper_command navigate_to_pose follow_path navigate_through_poses execute_trajectory move_group}"
ROUTE_B_BUILD_DIR="${ROUTE_B_BUILD_DIR:-${ROOT_DIR}/build-ohos-arm64-dds-source-deps}"
MANIFEST_PATH=""
WORK_ROOT="${WORK_ROOT:-${TMPDIR:-/tmp}/ibmw-ros2-interface-import-live-verify}"

usage() {
  cat <<EOF
Usage:
  bash scripts/verify_ros2_interface_import_live.sh [work-root]
  bash scripts/verify_ros2_interface_import_live.sh --manifest <migration-manifest.json> [work-root]

Environment:
  VERIFY_LANES       Space-separated lanes to run. Default: ${VERIFY_LANES}
                     Supported lanes: msg imported_srv reference_srv action route_b backend_metadata perf_boundary json_scaffold
  ACTION_RUN_CASES   Space-separated ACTION_CASE values for the action lane.
                      Default: ${ACTION_RUN_CASES}
  ROUTE_B_BUILD_DIR  Build dir passed to check_ohos_ros2_rosidl_compile.sh for route_b.
  IBMW_VERIFY_LANE_TIMEOUT_SECONDS
                      Optional default timeout, in seconds, for delegated lane
                      scripts. Unset means no wrapper-level timeout.
  IBMW_VERIFY_<LANE>_TIMEOUT_SECONDS
                      Optional per-lane override, e.g.
                      IBMW_VERIFY_ROUTE_B_TIMEOUT_SECONDS=1800.
  IBMW_JSON_SMOKE_IBMW_PREFIX
                     Optional installed IBMW prefix for json_scaffold. When unset,
                     the lane builds and installs a minimal prefix under its work dir;
                     HTTP smoke automatically enables the net extension for that prefix.
  IBMW_JSON_SMOKE_EXTRA_PREFIX
                     Optional extra CMAKE_PREFIX_PATH entry for generated scaffold deps.
                     Defaults to /opt/ros/humble when present.
  IBMW_JSON_SMOKE_HTTP_2PROC
                     Set to 1 to additionally run a two-process HTTP JSON
                     pub/sub smoke for selected cases. Requires net extension in the
                     IBMW prefix or IBMW_JSON_SMOKE_NET_EXTENSION.
  IBMW_JSON_SMOKE_NET_EXTENSION
                     Optional path to libibmw_net_transport.so for HTTP smoke.
  IBMW_JSON_SMOKE_HTTP_CASES
                     Space-separated JSON scaffold cases for HTTP smoke.
                     Default: plain. Use all for every generated smoke shape.
                     Supported: plain scalars fixed_array bounded dynamic nested_detail nested_top external_stamped.
  IBMW_JSON_SMOKE_REFERENCE_CASES
                     Space-separated real installed-prefix reference_existing JSON smoke cases.
                     Default: pose_stamped. Use all for the built-in reference matrix.
  IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES
                     Space-separated real reference_existing cases for HTTP smoke.
                     Default: empty. Use all for the built-in reference matrix.
                     Requested cases must also be selected by IBMW_JSON_SMOKE_REFERENCE_CASES.

When --manifest is provided, VERIFY_LANES defaults to
manifest.verification.selected_lanes unless VERIFY_LANES is already set.

This wrapper intentionally orchestrates existing evidence scripts. It does not
weaken the rcl-free runtime boundary: each delegated lane keeps its own live
checks, target audits, or server linkage guards.
EOF
}

fail() {
  echo "[FAIL] $*" >&2
  exit 1
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ "${1:-}" == "--manifest" ]]; then
  [[ -n "${2:-}" ]] || fail "--manifest requires a path"
  MANIFEST_PATH=$2
  WORK_ROOT="${3:-${WORK_ROOT}}"
else
  WORK_ROOT="${1:-${WORK_ROOT}}"
fi

if [[ -n "${MANIFEST_PATH}" && -z "${VERIFY_LANES_WAS_SET}" ]]; then
  [[ -f "${MANIFEST_PATH}" ]] || fail "manifest not found: ${MANIFEST_PATH}"
  VERIFY_LANES="$(${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${MANIFEST_PATH}" <<'PY'
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
verification = manifest.get("verification") or {}
lanes = verification.get("selected_lanes") or []
if not lanes:
    raise SystemExit("manifest verification.selected_lanes is empty or missing")
print(" ".join(lanes))
PY
)"
fi

mkdir -p "${WORK_ROOT}"

lane_timeout_seconds() {
  local lane=$1
  local lane_upper="${lane^^}"
  local var="IBMW_VERIFY_${lane_upper}_TIMEOUT_SECONDS"
  local value="${!var:-${IBMW_VERIFY_LANE_TIMEOUT_SECONDS:-}}"
  [[ -z "${value}" ]] && return 0
  [[ "${value}" =~ ^[0-9]+$ ]] || fail "${var} / IBMW_VERIFY_LANE_TIMEOUT_SECONDS must be an integer number of seconds"
  [[ "${value}" -gt 0 ]] || fail "${var} / IBMW_VERIFY_LANE_TIMEOUT_SECONDS must be greater than zero"
  printf '%s\n' "${value}"
}

run_with_lane_timeout() {
  local lane=$1
  shift
  local seconds
  seconds="$(lane_timeout_seconds "${lane}")"
  if [[ -n "${seconds}" ]]; then
    echo "[INFO] lane ${lane} command timeout: ${seconds}s"
    timeout "${seconds}s" "$@"
  else
    "$@"
  fi
}

verify_backend_metadata() {
  [[ -n "${MANIFEST_PATH}" ]] || fail "backend_metadata lane requires --manifest"
  [[ -f "${MANIFEST_PATH}" ]] || fail "manifest not found: ${MANIFEST_PATH}"
  ${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${MANIFEST_PATH}" <<'PY'
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1]).resolve()
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
manifest_dir = manifest_path.parent
metadata_files = manifest.get("backend_metadata_files") or []
generated_files = manifest.get("backend_generated_files") or []
status = manifest.get("target_backend_status") or {}
summary = status.get("summary") or {}
entries = {entry.get("target"): entry for entry in status.get("entries") or []}
errors = []

for rel in metadata_files:
    path = manifest_dir / rel
    if not path.is_file():
        errors.append(f"backend metadata file missing: {rel}")
        continue
    data = json.loads(path.read_text(encoding="utf-8"))
    target = data.get("target")
    if not target:
        errors.append(f"backend metadata file {rel} has no target")
        continue
    if data.get("metadata_only") is not True:
        errors.append(f"backend metadata file {rel} must set metadata_only=true")
    if data.get("runtime_lowering_generated") is not False:
        errors.append(f"backend metadata file {rel} must set runtime_lowering_generated=false")
    if data.get("status") != "degraded":
        errors.append(f"backend metadata file {rel} status must be degraded")
    if summary.get(target) != "degraded":
        errors.append(f"target_backend_status.summary.{target} must be degraded when {rel} exists")
    entry = entries.get(target) or {}
    artifacts = entry.get("generated_artifacts") or []
    if rel not in artifacts:
        errors.append(f"target_backend_status entry for {target} must list generated artifact {rel}")

for target, value in sorted(summary.items()):
    if value == "degraded":
        expected_suffix = f"_{target}_backend_metadata.json"
        if not any(rel.endswith(expected_suffix) for rel in metadata_files):
            errors.append(f"target {target} is degraded but no matching backend metadata sidecar is listed")

for rel in generated_files:
    path = manifest_dir / rel
    if not path.is_file():
        errors.append(f"backend generated file missing: {rel}")

if errors:
    raise SystemExit("\n".join(errors))
print(f"[PASS] backend metadata manifest consistency: {manifest_path}")
PY
}

verify_perf_boundary() {
  [[ -n "${MANIFEST_PATH}" ]] || fail "perf_boundary lane requires --manifest"
  [[ -f "${MANIFEST_PATH}" ]] || fail "manifest not found: ${MANIFEST_PATH}"
  local lane_work_dir=$1
  mkdir -p "${lane_work_dir}"
  ${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${MANIFEST_PATH}" "${lane_work_dir}" <<'PY'
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1]).resolve()
work_dir = pathlib.Path(sys.argv[2]).resolve()
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
manifest_dir = manifest_path.parent
status = manifest.get("target_backend_status") or {}
summary = status.get("summary") or {}
entries = {entry.get("target"): entry for entry in status.get("entries") or []}
metadata_files = manifest.get("backend_metadata_files") or []
generated_files = manifest.get("backend_generated_files") or []
errors = []

def require(condition, message):
    if not condition:
        errors.append(message)

dds_ros2 = entries.get("dds_ros2") or {}
if summary.get("dds_ros2") == "supported" or dds_ros2:
    require(dds_ros2.get("runtime_boundary") == "rcl_free_rosidl_fastrtps", "dds_ros2 must keep rcl_free_rosidl_fastrtps runtime boundary")

for target in ("dds_native", "iceoryx"):
    entry = entries.get(target) or {}
    target_status = summary.get(target)
    require(target_status != "supported", f"{target} must not be promoted by perf_boundary placeholder")
    if target_status == "degraded":
        require(entry.get("runtime_boundary", "").startswith("metadata_only_no_"), f"{target} degraded status must remain metadata-only")
        require(entry.get("generated_artifacts"), f"{target} degraded status must list generated metadata artifacts")

checked_metadata = []
zero_copy_sized_interfaces = 0
lowering_plan_files = []
zero_copy_targets = {"dds_native", "iceoryx"}
dds_native_metadata_seen = False
iceoryx_metadata_seen = False
for rel in metadata_files:
    path = manifest_dir / rel
    if not path.is_file():
        errors.append(f"backend metadata file missing: {rel}")
        continue
    data = json.loads(path.read_text(encoding="utf-8"))
    target = data.get("target")
    if target in zero_copy_targets:
        if target == "dds_native":
            dds_native_metadata_seen = True
        if target == "iceoryx":
            iceoryx_metadata_seen = True
        require(data.get("metadata_only") is True, f"{rel} must keep metadata_only=true")
        require(data.get("runtime_lowering_generated") is False, f"{rel} must keep runtime_lowering_generated=false until lowering gate lands")
        for interface in data.get("interfaces") or []:
            capability = interface.get("capability") or {}
            zero_copy = capability.get("zero_copy") or {}
            transports = capability.get("transports") or {}
            if target == "iceoryx":
                require(zero_copy.get("iceoryx") in {"yes", "no"}, f"{rel}:{interface.get('canonical')} must classify iceoryx zero-copy eligibility")
                if zero_copy.get("iceoryx") == "yes":
                    require(transports.get("iceoryx") == "yes", f"{rel}:{interface.get('canonical')} iceoryx zero-copy=yes requires transport iceoryx=yes")
                    require(
                        isinstance(capability.get("max_serialized_size_bytes"), int),
                        f"{rel}:{interface.get('canonical')} iceoryx zero-copy=yes requires max_serialized_size_bytes",
                    )
                    if isinstance(capability.get("max_serialized_size_bytes"), int):
                        zero_copy_sized_interfaces += 1
            if target == "dds_native":
                require(zero_copy.get("dds_loaned") in {"yes", "no"}, f"{rel}:{interface.get('canonical')} must classify DDS loaned eligibility")
        checked_metadata.append(rel)

if dds_native_metadata_seen:
    plan_rels = [rel for rel in generated_files if str(rel).endswith("_dds_native_lowering_plan.json")]
    require(plan_rels, "dds_native metadata requires *_dds_native_lowering_plan.json in backend_generated_files")
    for rel in plan_rels:
        path = manifest_dir / rel
        if not path.is_file():
            errors.append(f"dds_native lowering plan missing: {rel}")
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        require(data.get("target") == "dds_native", f"{rel} must target dds_native")
        require(data.get("artifact") == "dds_native_lowering_plan", f"{rel} must be a dds_native lowering plan")
        require(data.get("runtime_lowering_generated") is False, f"{rel} must keep runtime_lowering_generated=false")
        require(data.get("metadata_only") is True, f"{rel} must keep metadata_only=true")
        for entry in data.get("eligible_interfaces") or []:
            require(
                isinstance(entry.get("max_serialized_size_bytes"), int),
                f"{rel}:{entry.get('canonical')} eligible interface requires max_serialized_size_bytes",
            )
        lowering_plan_files.append(rel)

if iceoryx_metadata_seen:
    plan_rels = [rel for rel in generated_files if str(rel).endswith("_iceoryx_lowering_plan.json")]
    require(plan_rels, "iceoryx metadata requires *_iceoryx_lowering_plan.json in backend_generated_files")
    for rel in plan_rels:
        path = manifest_dir / rel
        if not path.is_file():
            errors.append(f"iceoryx lowering plan missing: {rel}")
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        require(data.get("target") == "iceoryx", f"{rel} must target iceoryx")
        require(data.get("artifact") == "iceoryx_lowering_plan", f"{rel} must be an iceoryx lowering plan")
        require(data.get("runtime_lowering_generated") is False, f"{rel} must keep runtime_lowering_generated=false")
        require(data.get("metadata_only") is True, f"{rel} must keep metadata_only=true")
        for entry in data.get("eligible_interfaces") or []:
            require(
                isinstance(entry.get("max_serialized_size_bytes"), int),
                f"{rel}:{entry.get('canonical')} eligible interface requires max_serialized_size_bytes",
            )
        lowering_plan_files.append(rel)

artifact = {
    "schema_version": 1,
    "lane": "perf_boundary",
    "artifact": "perf_boundary_summary",
    "manifest": str(manifest_path),
    "no_runtime_hot_path_changes": True,
    "uses_rcl_in_ibmw_runtime": False,
    "promotion_guard": "metadata_to_generated_artifact_to_compile_smoke_to_runtime_live_gate_to_status_promotion",
    "checked_targets": sorted(target for target in ("dds_ros2", "dds_native", "iceoryx") if target in summary or target in entries),
    "checked_backend_metadata_files": sorted(checked_metadata),
    "checked_lowering_plan_files": sorted(lowering_plan_files),
    "zero_copy_sized_interfaces": zero_copy_sized_interfaces,
    "status": "pass" if not errors else "fail",
    "errors": errors,
}
out = work_dir / "perf_boundary_summary.json"
out.write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n", encoding="utf-8")
if errors:
    raise SystemExit("\n".join(errors))
print(f"[PASS] perf boundary manifest guard: {manifest_path}")
print(f"[PASS] perf boundary summary: {out}")
PY
}

verify_json_scaffold_manifest() {
  [[ -n "${MANIFEST_PATH}" ]] || return 0
  [[ -f "${MANIFEST_PATH}" ]] || fail "manifest not found: ${MANIFEST_PATH}"
  ${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${MANIFEST_PATH}" <<'PY'
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1]).resolve()
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
manifest_dir = manifest_path.parent
generated_files = manifest.get("backend_generated_files") or []
verification = manifest.get("verification") or {}
lanes = verification.get("selected_lanes") or []
errors = []

if "json_scaffold" not in lanes:
    errors.append("manifest verification.selected_lanes must include json_scaffold for json_scaffold lane")
scaffolds = [rel for rel in generated_files if str(rel).endswith("_json_runtime_scaffold.ibmw.yaml")]
types_headers = [rel for rel in generated_files if str(rel).endswith("_json_types.hpp")]
if not scaffolds:
    errors.append("manifest backend_generated_files must list *_json_runtime_scaffold.ibmw.yaml")
if not types_headers:
    errors.append("manifest backend_generated_files must list *_json_types.hpp")
for rel in scaffolds + types_headers:
    if not (manifest_dir / rel).is_file():
        errors.append(f"manifest-listed JSON generated file missing: {rel}")

if errors:
    raise SystemExit("\n".join(errors))
print(f"[PASS] json scaffold manifest artifacts: {manifest_path}")
PY
}

verify_json_scaffold_runtime() {
  local lane_work_dir=$1
  local py="${PYTHON_EXECUTABLE:-/usr/bin/python3}"
  local ibmw_prefix="${IBMW_JSON_SMOKE_IBMW_PREFIX:-}"
  local extra_prefix="${IBMW_JSON_SMOKE_EXTRA_PREFIX:-}"
  local all_generated_cases="plain scalars fixed_array bounded dynamic nested_detail nested_top external_stamped"
  local http_cases=""

  mkdir -p "${lane_work_dir}"

  if [[ -z "${extra_prefix}" && -d /opt/ros/humble ]]; then
    extra_prefix=/opt/ros/humble
  fi

  if [[ -z "${ibmw_prefix}" ]]; then
    ibmw_prefix="${lane_work_dir}/ibmw_install"
    local build_net_extension=OFF
    if [[ "${IBMW_JSON_SMOKE_HTTP_2PROC:-0}" == "1" ]]; then
      build_net_extension=ON
    fi
    cmake -S "${ROOT_DIR}" -B "${lane_work_dir}/ibmw_build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DIBMW_BUILD_TESTS=OFF \
      -DIBMW_BUILD_RUNTIME=ON \
      -DIBMW_BUILD_PROTOCOLS=ON \
      -DIBMW_BUILD_NET_EXTENSION="${build_net_extension}" \
      -DIBMW_BUILD_CLI_TOOLS=OFF \
      -DIBMW_BUILD_WITH_ROS2=OFF \
      -DIBMW_BUILD_EXAMPLES=OFF \
      -DIBMW_USE_FMT_LIB=ON \
      -DIBMW_ENABLE_DLOPEN_DEEPBIND=ON \
      -DIBMW_CONTEXT_DEFINE_GLOBAL_LOGGER_HANDLE=ON \
      -DIBMW_BUILD_WITH_WERROR=ON \
      -DCMAKE_INSTALL_PREFIX="${ibmw_prefix}"
    cmake --build "${lane_work_dir}/ibmw_build" -j"$(nproc)"
    cmake --install "${lane_work_dir}/ibmw_build"
  fi

  [[ -f "${ibmw_prefix}/lib/cmake/ibmw/ibmw-config.cmake" ]] || \
    fail "json_scaffold lane could not find ibmw-config.cmake under ${ibmw_prefix}"

  "${py}" - "${ROOT_DIR}" "${lane_work_dir}" <<'PY'
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
work = pathlib.Path(sys.argv[2])
for dep_pkg in ("builtin_interfaces", "std_msgs"):
    dep = work / dep_pkg
    if dep.exists():
        shutil.rmtree(dep)
    (dep / "msg").mkdir(parents=True)
    (dep / "package.xml").write_text(
        f"<package format='3'><name>{dep_pkg}</name><version>0.1.0</version></package>\n",
        encoding="utf-8",
    )
(work / "builtin_interfaces" / "msg" / "Time.msg").write_text(
    "int32 sec\nuint32 nanosec\n",
    encoding="utf-8",
)
(work / "std_msgs" / "msg" / "Header.msg").write_text(
    "builtin_interfaces/Time stamp\nstring frame_id\n",
    encoding="utf-8",
)
src = work / "demo_interfaces"
if src.exists():
    shutil.rmtree(src)
(src / "msg").mkdir(parents=True)
(src / "package.xml").write_text(
    "<package format='3'><name>demo_interfaces</name><version>0.1.0</version></package>\n",
    encoding="utf-8",
)
(src / "msg" / "Plain.msg").write_text(
    "float64 x\nfloat64 y\nint32[3] rgb\n",
    encoding="utf-8",
)
(src / "msg" / "Scalars.msg").write_text(
    "bool enabled\nint32 count\nfloat32 ratio\nuint64 stamp\n",
    encoding="utf-8",
)
(src / "msg" / "FixedArray.msg").write_text(
    "uint8[4] bytes\nfloat64[2] pose\n",
    encoding="utf-8",
)
(src / "msg" / "Bounded.msg").write_text(
    "string<=16 label\nuint8[<=8] payload\nfloat64[<=2] samples\n",
    encoding="utf-8",
)
(src / "msg" / "Dynamic.msg").write_text(
    "string name\nuint8[] payload\nfloat64[] samples\n",
    encoding="utf-8",
)
(src / "msg" / "ExternalStamped.msg").write_text(
    "std_msgs/Header header\nfloat64 value\n",
    encoding="utf-8",
)
(src / "msg" / "NestedDetail.msg").write_text(
    "float64 value\nint32[2] bins\n",
    encoding="utf-8",
)
(src / "msg" / "NestedTop.msg").write_text(
    "NestedDetail detail\nbool ok\n",
    encoding="utf-8",
)
sys.path.insert(0, str(root / "src" / "tools" / "ibmw_gen"))
from ros2_interface_import import emit_rosidl_artifact_package

imported = work / "imported"
generated = work / "generated_project"
shutil.rmtree(imported, ignore_errors=True)
shutil.rmtree(generated, ignore_errors=True)
emit_rosidl_artifact_package(str(src), str(imported), targets=["json"])
subprocess.run(
    [
        sys.executable,
        str(root / "src" / "tools" / "ibmw_gen" / "ibmw_gen.py"),
        str(imported / "demo_interfaces_json_runtime_scaffold.ibmw.yaml"),
        "-o",
        str(generated),
        "--force",
    ],
    check=True,
)
PY

  local cmake_prefix="${ibmw_prefix}"
  if [[ -n "${extra_prefix}" ]]; then
    cmake_prefix="${cmake_prefix};${extra_prefix}"
  fi
  cmake -S "${lane_work_dir}/generated_project" -B "${lane_work_dir}/generated_project_build" \
    -DCMAKE_PREFIX_PATH="${cmake_prefix}" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "${lane_work_dir}/generated_project_build" -j"$(nproc)"

  cat > "${lane_work_dir}/generated_project_build/cfg/plain_json_combined_pkg.yaml" <<'YAML'
ibmw:
  logging:
    core_level: INFO
    backends:
      - type: console

  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2

  messaging:
    drivers:
      - type: local
    publishers:
      - topic: "/bounded"
        enable_drivers: [local]
      - topic: "/dynamic"
        enable_drivers: [local]
      - topic: "/external_stamped"
        enable_drivers: [local]
      - topic: "/fixed_array"
        enable_drivers: [local]
      - topic: "/nested_detail"
        enable_drivers: [local]
      - topic: "/nested_top"
        enable_drivers: [local]
      - topic: "/plain"
        enable_drivers: [local]
      - topic: "/scalars"
        enable_drivers: [local]
    subscribers:
      - topic: "/bounded"
        enable_drivers: [local]
      - topic: "/dynamic"
        enable_drivers: [local]
      - topic: "/external_stamped"
        enable_drivers: [local]
      - topic: "/fixed_array"
        enable_drivers: [local]
      - topic: "/nested_detail"
        enable_drivers: [local]
      - topic: "/nested_top"
        enable_drivers: [local]
      - topic: "/plain"
        enable_drivers: [local]
      - topic: "/scalars"
        enable_drivers: [local]

  modules:
    packages:
      - path: libbounded_json_publisher_module_pkg.so
        enable_modules: [BoundedJsonPublisherModule]
      - path: libbounded_json_subscriber_module_pkg.so
        enable_modules: [BoundedJsonSubscriberModule]
      - path: libdynamic_json_publisher_module_pkg.so
        enable_modules: [DynamicJsonPublisherModule]
      - path: libdynamic_json_subscriber_module_pkg.so
        enable_modules: [DynamicJsonSubscriberModule]
      - path: libexternal_stamped_json_publisher_module_pkg.so
        enable_modules: [ExternalStampedJsonPublisherModule]
      - path: libexternal_stamped_json_subscriber_module_pkg.so
        enable_modules: [ExternalStampedJsonSubscriberModule]
      - path: libfixed_array_json_publisher_module_pkg.so
        enable_modules: [FixedArrayJsonPublisherModule]
      - path: libfixed_array_json_subscriber_module_pkg.so
        enable_modules: [FixedArrayJsonSubscriberModule]
      - path: libnested_detail_json_publisher_module_pkg.so
        enable_modules: [NestedDetailJsonPublisherModule]
      - path: libnested_detail_json_subscriber_module_pkg.so
        enable_modules: [NestedDetailJsonSubscriberModule]
      - path: libnested_top_json_publisher_module_pkg.so
        enable_modules: [NestedTopJsonPublisherModule]
      - path: libnested_top_json_subscriber_module_pkg.so
        enable_modules: [NestedTopJsonSubscriberModule]
      - path: libplain_json_publisher_module_pkg.so
        enable_modules: [PlainJsonPublisherModule]
      - path: libplain_json_subscriber_module_pkg.so
        enable_modules: [PlainJsonSubscriberModule]
      - path: libscalars_json_publisher_module_pkg.so
        enable_modules: [ScalarsJsonPublisherModule]
      - path: libscalars_json_subscriber_module_pkg.so
        enable_modules: [ScalarsJsonSubscriberModule]
    modules:
      - name: BoundedJsonPublisherModule
        log_level: INFO
      - name: BoundedJsonSubscriberModule
        log_level: INFO
      - name: DynamicJsonPublisherModule
        log_level: INFO
      - name: DynamicJsonSubscriberModule
        log_level: INFO
      - name: ExternalStampedJsonPublisherModule
        log_level: INFO
      - name: ExternalStampedJsonSubscriberModule
        log_level: INFO
      - name: FixedArrayJsonPublisherModule
        log_level: INFO
      - name: FixedArrayJsonSubscriberModule
        log_level: INFO
      - name: NestedDetailJsonPublisherModule
        log_level: INFO
      - name: NestedDetailJsonSubscriberModule
        log_level: INFO
      - name: NestedTopJsonPublisherModule
        log_level: INFO
      - name: NestedTopJsonSubscriberModule
        log_level: INFO
      - name: PlainJsonPublisherModule
        log_level: INFO
      - name: PlainJsonSubscriberModule
        log_level: INFO
      - name: ScalarsJsonPublisherModule
        log_level: INFO
      - name: ScalarsJsonSubscriberModule
        log_level: INFO
YAML

  local log_path="${lane_work_dir}/json_scaffold_runtime.log"
  set +e
  (cd "${lane_work_dir}/generated_project_build" && timeout 8s "${ibmw_prefix}/bin/ibmw" run cfg/plain_json_combined_pkg.yaml > "${log_path}" 2>&1)
  local smoke_rc=$?
  set -e
  [[ "${smoke_rc}" -eq 0 || "${smoke_rc}" -eq 124 ]] || fail "json_scaffold runtime smoke failed with rc=${smoke_rc}; see ${log_path}"
  "${py}" - "${log_path}" <<'PY'
import pathlib
import sys

log_path = pathlib.Path(sys.argv[1])
log = log_path.read_text(encoding="utf-8", errors="replace")
checks = {
    "bounded_publisher_setup": "BoundedJsonPublisherModule::Setup succeeded" in log,
    "bounded_subscriber_setup": "BoundedJsonSubscriberModule::Setup succeeded" in log,
    "dynamic_publisher_setup": "DynamicJsonPublisherModule::Setup succeeded" in log,
    "dynamic_subscriber_setup": "DynamicJsonSubscriberModule::Setup succeeded" in log,
    "external_stamped_publisher_setup": "ExternalStampedJsonPublisherModule::Setup succeeded" in log,
    "external_stamped_subscriber_setup": "ExternalStampedJsonSubscriberModule::Setup succeeded" in log,
    "fixed_array_publisher_setup": "FixedArrayJsonPublisherModule::Setup succeeded" in log,
    "fixed_array_subscriber_setup": "FixedArrayJsonSubscriberModule::Setup succeeded" in log,
    "nested_detail_publisher_setup": "NestedDetailJsonPublisherModule::Setup succeeded" in log,
    "nested_detail_subscriber_setup": "NestedDetailJsonSubscriberModule::Setup succeeded" in log,
    "nested_top_publisher_setup": "NestedTopJsonPublisherModule::Setup succeeded" in log,
    "nested_top_subscriber_setup": "NestedTopJsonSubscriberModule::Setup succeeded" in log,
    "publisher_setup": "PlainJsonPublisherModule::Setup succeeded" in log,
    "subscriber_setup": "PlainJsonSubscriberModule::Setup succeeded" in log,
    "scalars_publisher_setup": "ScalarsJsonPublisherModule::Setup succeeded" in log,
    "scalars_subscriber_setup": "ScalarsJsonSubscriberModule::Setup succeeded" in log,
    "published": "Published message #" in log,
    "received": "Received message." in log,
    "bounded_type_registered": "json:demo_interfaces/msg/Bounded" in log,
    "dynamic_type_registered": "json:demo_interfaces/msg/Dynamic" in log,
    "external_stamped_type_registered": "json:demo_interfaces/msg/ExternalStamped" in log,
    "fixed_array_type_registered": "json:demo_interfaces/msg/FixedArray" in log,
    "nested_detail_type_registered": "json:demo_interfaces/msg/NestedDetail" in log,
    "nested_top_type_registered": "json:demo_interfaces/msg/NestedTop" in log,
    "type_registered": "json:demo_interfaces/msg/Plain" in log,
    "scalars_type_registered": "json:demo_interfaces/msg/Scalars" in log,
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    raise SystemExit(f"json scaffold runtime log missing {missing}: {log_path}")
print(f"[PASS] json scaffold compile/runtime smoke: {log_path}")
PY

  "${py}" - "${ROOT_DIR}" "${lane_work_dir}" <<'PY'
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
work = pathlib.Path(sys.argv[2])
prefix = work / "reference_existing_prefix"
for pkg_name in ("builtin_interfaces", "std_msgs"):
    pkg = prefix / "share" / pkg_name
    shutil.rmtree(pkg, ignore_errors=True)
    (pkg / "msg").mkdir(parents=True, exist_ok=True)
    (pkg / "package.xml").write_text(
        f"<package format='3'><name>{pkg_name}</name><version>0.1.0</version></package>\n",
        encoding="utf-8",
    )
(prefix / "share" / "builtin_interfaces" / "msg" / "Time.msg").write_text(
    "int32 sec\nuint32 nanosec\n",
    encoding="utf-8",
)
(prefix / "share" / "std_msgs" / "msg" / "Header.msg").write_text(
    "builtin_interfaces/Time stamp\nstring frame_id\n",
    encoding="utf-8",
)
sys.path.insert(0, str(root / "src" / "tools" / "ibmw_gen"))
from ros2_interface_import import emit_reference_existing_interface_report

imported = work / "reference_existing_imported"
generated = work / "reference_existing_generated_project"
shutil.rmtree(imported, ignore_errors=True)
shutil.rmtree(generated, ignore_errors=True)
emit_reference_existing_interface_report(
    str(prefix),
    "std_msgs",
    str(imported),
    targets=["json"],
    selected_types=["msg/Header"],
)
subprocess.run(
    [
        sys.executable,
        str(root / "src" / "tools" / "ibmw_gen" / "ibmw_gen.py"),
        str(imported / "std_msgs_json_runtime_scaffold.ibmw.yaml"),
        "-o",
        str(generated),
        "--force",
    ],
    check=True,
)
PY

  local nlohmann_json_source="${lane_work_dir}/generated_project_build/_deps/nlohmann_json-src"
  local reference_cmake_dep_args=()
  if [[ -d "${nlohmann_json_source}" ]]; then
    reference_cmake_dep_args+=("-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${nlohmann_json_source}")
  fi
  cmake -S "${lane_work_dir}/reference_existing_generated_project" -B "${lane_work_dir}/reference_existing_generated_project_build" \
    -DCMAKE_PREFIX_PATH="${cmake_prefix}" \
    -DCMAKE_BUILD_TYPE=Release \
    "${reference_cmake_dep_args[@]}"
  cmake --build "${lane_work_dir}/reference_existing_generated_project_build" -j"$(nproc)"

  cat > "${lane_work_dir}/reference_existing_generated_project_build/cfg/header_json_combined_pkg.yaml" <<'YAML'
ibmw:
  logging:
    core_level: INFO
    backends:
      - type: console

  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2

  messaging:
    drivers:
      - type: local
    publishers:
      - topic: "/header"
        enable_drivers: [local]
    subscribers:
      - topic: "/header"
        enable_drivers: [local]

  modules:
    packages:
      - path: libheader_json_publisher_module_pkg.so
        enable_modules: [HeaderJsonPublisherModule]
      - path: libheader_json_subscriber_module_pkg.so
        enable_modules: [HeaderJsonSubscriberModule]
    modules:
      - name: HeaderJsonPublisherModule
        log_level: INFO
      - name: HeaderJsonSubscriberModule
        log_level: INFO
YAML

  local reference_log_path="${lane_work_dir}/reference_existing_json_scaffold_runtime.log"
  set +e
  (cd "${lane_work_dir}/reference_existing_generated_project_build" && timeout 8s "${ibmw_prefix}/bin/ibmw" run cfg/header_json_combined_pkg.yaml > "${reference_log_path}" 2>&1)
  local reference_smoke_rc=$?
  set -e
  [[ "${reference_smoke_rc}" -eq 0 || "${reference_smoke_rc}" -eq 124 ]] || fail "reference_existing json_scaffold runtime smoke failed with rc=${reference_smoke_rc}; see ${reference_log_path}"
  "${py}" - "${reference_log_path}" <<'PY'
import pathlib
import sys

log_path = pathlib.Path(sys.argv[1])
log = log_path.read_text(encoding="utf-8", errors="replace")
checks = {
    "header_publisher_setup": "HeaderJsonPublisherModule::Setup succeeded" in log,
    "header_subscriber_setup": "HeaderJsonSubscriberModule::Setup succeeded" in log,
    "header_type_registered": "json:std_msgs/msg/Header" in log,
    "published": "Published message #" in log,
    "received": "Received message." in log,
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    raise SystemExit(f"reference_existing json scaffold runtime log missing {missing}: {log_path}")
print(f"[PASS] reference_existing json scaffold runtime smoke: {log_path}")
PY

  local all_reference_cases=""
  all_reference_cases+="pose_stamped odometry imu "
  all_reference_cases+="transform_stamped twist laser_scan "
  all_reference_cases+="path point_cloud2 nav_sat_fix pose_with_covariance_stamped "
  all_reference_cases+="point_stamped vector3_stamped wrench_stamped accel_stamped image "
  all_reference_cases+="camera_info joint_state battery_state range fluid_pressure temperature "
  all_reference_cases+="occupancy_grid map_meta_data"
  local reference_cases="${IBMW_JSON_SMOKE_REFERENCE_CASES:-pose_stamped}"
  if [[ "${reference_cases}" == "all" ]]; then
    reference_cases="${all_reference_cases}"
  fi
  local http_reference_cases=""
  local reference_case
  for reference_case in ${reference_cases}; do
    set +e
    "${py}" - "${ROOT_DIR}" "${lane_work_dir}" "${reference_case}" <<'PY'
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
work = pathlib.Path(sys.argv[2])
case = sys.argv[3]
case_info = {
    "imu": {
        "package": "sensor_msgs",
        "selector": "msg/Imu",
        "type_name": "sensor_msgs/msg/Imu",
        "class_name": "Imu",
        "snake": "imu",
    },
    "odometry": {
        "package": "nav_msgs",
        "selector": "msg/Odometry",
        "type_name": "nav_msgs/msg/Odometry",
        "class_name": "Odometry",
        "snake": "odometry",
    },
    "path": {
        "package": "nav_msgs",
        "selector": "msg/Path",
        "type_name": "nav_msgs/msg/Path",
        "class_name": "Path",
        "snake": "path",
    },
    "pose_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/PoseStamped",
        "type_name": "geometry_msgs/msg/PoseStamped",
        "class_name": "PoseStamped",
        "snake": "pose_stamped",
    },
    "point_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/PointStamped",
        "type_name": "geometry_msgs/msg/PointStamped",
        "class_name": "PointStamped",
        "snake": "point_stamped",
    },
    "vector3_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/Vector3Stamped",
        "type_name": "geometry_msgs/msg/Vector3Stamped",
        "class_name": "Vector3Stamped",
        "snake": "vector3_stamped",
    },
    "wrench_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/WrenchStamped",
        "type_name": "geometry_msgs/msg/WrenchStamped",
        "class_name": "WrenchStamped",
        "snake": "wrench_stamped",
    },
    "accel_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/AccelStamped",
        "type_name": "geometry_msgs/msg/AccelStamped",
        "class_name": "AccelStamped",
        "snake": "accel_stamped",
    },
    "transform_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/TransformStamped",
        "type_name": "geometry_msgs/msg/TransformStamped",
        "class_name": "TransformStamped",
        "snake": "transform_stamped",
    },
    "twist": {
        "package": "geometry_msgs",
        "selector": "msg/Twist",
        "type_name": "geometry_msgs/msg/Twist",
        "class_name": "Twist",
        "snake": "twist",
    },
    "point_cloud2": {
        "package": "sensor_msgs",
        "selector": "msg/PointCloud2",
        "type_name": "sensor_msgs/msg/PointCloud2",
        "class_name": "PointCloud2",
        "snake": "point_cloud2",
    },
    "nav_sat_fix": {
        "package": "sensor_msgs",
        "selector": "msg/NavSatFix",
        "type_name": "sensor_msgs/msg/NavSatFix",
        "class_name": "NavSatFix",
        "snake": "nav_sat_fix",
    },
    "pose_with_covariance_stamped": {
        "package": "geometry_msgs",
        "selector": "msg/PoseWithCovarianceStamped",
        "type_name": "geometry_msgs/msg/PoseWithCovarianceStamped",
        "class_name": "PoseWithCovarianceStamped",
        "snake": "pose_with_covariance_stamped",
    },
    "laser_scan": {
        "package": "sensor_msgs",
        "selector": "msg/LaserScan",
        "type_name": "sensor_msgs/msg/LaserScan",
        "class_name": "LaserScan",
        "snake": "laser_scan",
    },
    "image": {
        "package": "sensor_msgs",
        "selector": "msg/Image",
        "type_name": "sensor_msgs/msg/Image",
        "class_name": "Image",
        "snake": "image",
    },
    "camera_info": {
        "package": "sensor_msgs",
        "selector": "msg/CameraInfo",
        "type_name": "sensor_msgs/msg/CameraInfo",
        "class_name": "CameraInfo",
        "snake": "camera_info",
    },
    "joint_state": {
        "package": "sensor_msgs",
        "selector": "msg/JointState",
        "type_name": "sensor_msgs/msg/JointState",
        "class_name": "JointState",
        "snake": "joint_state",
    },
    "battery_state": {
        "package": "sensor_msgs",
        "selector": "msg/BatteryState",
        "type_name": "sensor_msgs/msg/BatteryState",
        "class_name": "BatteryState",
        "snake": "battery_state",
    },
    "range": {
        "package": "sensor_msgs",
        "selector": "msg/Range",
        "type_name": "sensor_msgs/msg/Range",
        "class_name": "Range",
        "snake": "range",
    },
    "fluid_pressure": {
        "package": "sensor_msgs",
        "selector": "msg/FluidPressure",
        "type_name": "sensor_msgs/msg/FluidPressure",
        "class_name": "FluidPressure",
        "snake": "fluid_pressure",
    },
    "temperature": {
        "package": "sensor_msgs",
        "selector": "msg/Temperature",
        "type_name": "sensor_msgs/msg/Temperature",
        "class_name": "Temperature",
        "snake": "temperature",
    },
    "occupancy_grid": {
        "package": "nav_msgs",
        "selector": "msg/OccupancyGrid",
        "type_name": "nav_msgs/msg/OccupancyGrid",
        "class_name": "OccupancyGrid",
        "snake": "occupancy_grid",
    },
    "map_meta_data": {
        "package": "nav_msgs",
        "selector": "msg/MapMetaData",
        "type_name": "nav_msgs/msg/MapMetaData",
        "class_name": "MapMetaData",
        "snake": "map_meta_data",
    },
}
if case not in case_info:
    raise SystemExit("unknown IBMW_JSON_SMOKE_REFERENCE_CASES entry: " + case)
info = case_info[case]
prefix = pathlib.Path("/opt/ros/humble")
msg_path = prefix / "share" / info["package"] / "msg" / (info["selector"].split("/", 1)[1] + ".msg")
if not msg_path.is_file():
    print(f"[SKIP] real reference_existing json scaffold runtime smoke ({case}): {msg_path} not found")
    raise SystemExit(2)

imported = work / f"real_reference_existing_{case}_imported"
generated = work / f"real_reference_existing_{case}_generated_project"
shutil.rmtree(imported, ignore_errors=True)
shutil.rmtree(generated, ignore_errors=True)
sys.path.insert(0, str(root / "src" / "tools" / "ibmw_gen"))
from ros2_interface_import import emit_reference_existing_interface_report

emit_reference_existing_interface_report(
    str(prefix),
    info["package"],
    str(imported),
    targets=["json"],
    selected_types=[info["selector"]],
)
subprocess.run(
    [
        sys.executable,
        str(root / "src" / "tools" / "ibmw_gen" / "ibmw_gen.py"),
        str(imported / f"{info['package']}_json_runtime_scaffold.ibmw.yaml"),
        "-o",
        str(generated),
        "--force",
    ],
    check=True,
)
(work / f"real_reference_existing_{case}_metadata.txt").write_text(
    "\n".join([info["package"], info["type_name"], info["class_name"], info["snake"]]) + "\n",
    encoding="utf-8",
)
PY
    local py_rc=$?
    set -e
    if [[ "${py_rc}" -eq 2 ]]; then
      continue
    fi
    [[ "${py_rc}" -eq 0 ]] || fail "real reference_existing json scaffold generation failed for ${reference_case} with rc=${py_rc}"
    local reference_metadata="${lane_work_dir}/real_reference_existing_${reference_case}_metadata.txt"
    [[ -f "${reference_metadata}" ]] || fail "real reference_existing metadata missing for ${reference_case}"
    mapfile -t reference_info < "${reference_metadata}"
    local reference_type_name="${reference_info[1]}"
    local reference_class_name="${reference_info[2]}"
    local reference_snake="${reference_info[3]}"
    local reference_build_dir="${lane_work_dir}/real_reference_existing_${reference_case}_generated_project_build"
    cmake -S "${lane_work_dir}/real_reference_existing_${reference_case}_generated_project" -B "${reference_build_dir}" \
      -DCMAKE_PREFIX_PATH="${cmake_prefix}" \
      -DCMAKE_BUILD_TYPE=Release \
      "${reference_cmake_dep_args[@]}"
    cmake --build "${reference_build_dir}" -j"$(nproc)"

    "${py}" - "${reference_build_dir}" "${reference_snake}" "${reference_class_name}" <<'PY'
import pathlib
import sys

build = pathlib.Path(sys.argv[1])
snake = sys.argv[2]
class_name = sys.argv[3]
cfg = f'''ibmw:
  logging:
    core_level: INFO
    backends:
      - type: console

  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2

  messaging:
    drivers:
      - type: local
    publishers:
      - topic: "/{snake}"
        enable_drivers: [local]
    subscribers:
      - topic: "/{snake}"
        enable_drivers: [local]

  modules:
    packages:
      - path: lib{snake}_json_publisher_module_pkg.so
        enable_modules: [{class_name}JsonPublisherModule]
      - path: lib{snake}_json_subscriber_module_pkg.so
        enable_modules: [{class_name}JsonSubscriberModule]
    modules:
      - name: {class_name}JsonPublisherModule
        log_level: INFO
      - name: {class_name}JsonSubscriberModule
        log_level: INFO
'''
(build / "cfg" / f"{snake}_json_combined_pkg.yaml").write_text(cfg, encoding="utf-8")
PY

    local real_reference_log_path="${lane_work_dir}/real_reference_existing_${reference_case}_json_scaffold_runtime.log"
    set +e
    (cd "${reference_build_dir}" && timeout 8s "${ibmw_prefix}/bin/ibmw" run "cfg/${reference_snake}_json_combined_pkg.yaml" > "${real_reference_log_path}" 2>&1)
    local real_reference_smoke_rc=$?
    set -e
    [[ "${real_reference_smoke_rc}" -eq 0 || "${real_reference_smoke_rc}" -eq 124 ]] || fail "real reference_existing json_scaffold runtime smoke failed for ${reference_case} with rc=${real_reference_smoke_rc}; see ${real_reference_log_path}"
    "${py}" - "${reference_case}" "${reference_type_name}" "${reference_class_name}" "${real_reference_log_path}" <<'PY'
import pathlib
import sys

case = sys.argv[1]
type_name = sys.argv[2]
class_name = sys.argv[3]
log_path = pathlib.Path(sys.argv[4])
log = log_path.read_text(encoding="utf-8", errors="replace")
checks = {
    "publisher_setup": f"{class_name}JsonPublisherModule::Setup succeeded" in log,
    "subscriber_setup": f"{class_name}JsonSubscriberModule::Setup succeeded" in log,
    "type_registered": f"json:{type_name}" in log,
    "published": "Published message #" in log,
    "received": "Received message." in log,
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    raise SystemExit(f"real reference_existing json scaffold runtime log missing {missing} for {case}: {log_path}")
print(f"[PASS] real reference_existing json scaffold runtime smoke ({case}): {log_path}")
PY
  done

  if [[ "${IBMW_JSON_SMOKE_HTTP_2PROC:-0}" == "1" ]]; then
    local net_extension="${IBMW_JSON_SMOKE_NET_EXTENSION:-}"
    if [[ -z "${net_extension}" && -f "${ibmw_prefix}/bin/libibmw_net_transport.so" ]]; then
      net_extension="${ibmw_prefix}/bin/libibmw_net_transport.so"
    fi
    if [[ -z "${net_extension}" && -f "${ibmw_prefix}/lib/libibmw_net_transport.so" ]]; then
      net_extension="${ibmw_prefix}/lib/libibmw_net_transport.so"
    fi
    [[ -f "${net_extension}" ]] || fail "json HTTP 2-process smoke requires libibmw_net_transport.so; set IBMW_JSON_SMOKE_NET_EXTENSION or use a net-enabled IBMW prefix"

    local http_port="${IBMW_JSON_SMOKE_HTTP_PORT:-50181}"
    http_cases="${IBMW_JSON_SMOKE_HTTP_CASES:-plain}"
    if [[ "${http_cases}" == "all" ]]; then
      http_cases="${all_generated_cases}"
    fi
    "${py}" - "${lane_work_dir}/generated_project_build" "${net_extension}" "${http_port}" ${http_cases} <<'PY'
import pathlib
import sys

build = pathlib.Path(sys.argv[1])
net_extension = sys.argv[2]
base_port = int(sys.argv[3])
cases = sys.argv[4:] or ["plain"]
cfg = build / "cfg"
case_message = {
    "bounded": "Bounded",
    "dynamic": "Dynamic",
    "external_stamped": "ExternalStamped",
    "fixed_array": "FixedArray",
    "nested_detail": "NestedDetail",
    "nested_top": "NestedTop",
    "plain": "Plain",
    "scalars": "Scalars",
}
unknown = sorted(set(cases) - set(case_message))
if unknown:
    raise SystemExit("unknown IBMW_JSON_SMOKE_HTTP_CASES entries: " + ", ".join(unknown))

common = f'''  extensions:
    items:
      - name: net_extension
        path: {net_extension}
        options:
          thread_num: 2
          http_options:
            listen_ip: "127.0.0.1"
            listen_port: {{port}}

  logging:
    core_level: INFO
    backends:
      - type: console

  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2
'''

def pascal_to_snake(name: str) -> str:
    out = []
    for index, char in enumerate(name):
        if char.isupper() and index:
            out.append("_")
        out.append(char.lower())
    return "".join(out)

for index, case in enumerate(cases):
    msg = case_message[case]
    snake = pascal_to_snake(msg)
    topic = f"/{snake}"
    sub_port = str(base_port + index * 2)
    pub_port = str(base_port + index * 2 + 1)
    sub_class = f"{msg}JsonSubscriberModule"
    pub_class = f"{msg}JsonPublisherModule"
    sub = 'ibmw:\n' + common.format(port=sub_port) + f'''
  messaging:
    drivers:
      - type: http
        options:
          subscribers:
            - topic: "{topic}"
    publishers: []
    subscribers:
      - topic: "{topic}"
        enable_drivers: [http]

  modules:
    packages:
      - path: lib{snake}_json_subscriber_module_pkg.so
        enable_modules: [{sub_class}]
    modules:
      - name: {sub_class}
        log_level: INFO
'''
    pub = 'ibmw:\n' + common.format(port=pub_port) + f'''
  messaging:
    drivers:
      - type: http
        options:
          publishers:
            - topic: "{topic}"
              server_url_list: ["127.0.0.1:{sub_port}"]
    publishers:
      - topic: "{topic}"
        enable_drivers: [http]
    subscribers: []

  modules:
    packages:
      - path: lib{snake}_json_publisher_module_pkg.so
        enable_modules: [{pub_class}]
    modules:
      - name: {pub_class}
        log_level: INFO
'''
    (cfg / f"{case}_json_http_subscriber_pkg.yaml").write_text(sub, encoding="utf-8")
    (cfg / f"{case}_json_http_publisher_pkg.yaml").write_text(pub, encoding="utf-8")
PY

    local http_case
    for http_case in ${http_cases}; do
      local http_sub_log="${lane_work_dir}/json_scaffold_http_${http_case}_subscriber.log"
      local http_pub_log="${lane_work_dir}/json_scaffold_http_${http_case}_publisher.log"
      set +e
      (
        cd "${lane_work_dir}/generated_project_build"
        timeout 10s "${ibmw_prefix}/bin/ibmw" run "cfg/${http_case}_json_http_subscriber_pkg.yaml" > "${http_sub_log}" 2>&1
      ) &
      local sub_pid=$!
      sleep 1
      (
        cd "${lane_work_dir}/generated_project_build"
        timeout 5s "${ibmw_prefix}/bin/ibmw" run "cfg/${http_case}_json_http_publisher_pkg.yaml" > "${http_pub_log}" 2>&1
      )
      local pub_rc=$?
      wait "${sub_pid}"
      local sub_rc=$?
      set -e
      [[ "${pub_rc}" -eq 0 || "${pub_rc}" -eq 124 ]] || fail "json HTTP publisher smoke failed for ${http_case} with rc=${pub_rc}; see ${http_pub_log}"
      [[ "${sub_rc}" -eq 0 || "${sub_rc}" -eq 124 ]] || fail "json HTTP subscriber smoke failed for ${http_case} with rc=${sub_rc}; see ${http_sub_log}"
      "${py}" - "${http_case}" "${http_pub_log}" "${http_sub_log}" <<'PY'
import pathlib
import sys

case = sys.argv[1]
pub = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8", errors="replace")
sub = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8", errors="replace")
checks = {
    "http_published": "Published message #" in pub,
    "http_received": "Received message." in sub,
    "http_driver_registered": "drivers: http" in sub or "http" in sub.lower(),
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    raise SystemExit(f"json HTTP 2-process smoke missing evidence for {case}: " + ", ".join(missing))
print(f"[PASS] json scaffold HTTP 2-process smoke ({case}): {sys.argv[2]} {sys.argv[3]}")
PY
    done

    http_reference_cases="${IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES:-}"
    if [[ -n "${http_reference_cases}" ]]; then
      if [[ "${http_reference_cases}" == "all" ]]; then
        http_reference_cases="${all_reference_cases}"
      fi
      local http_reference_index=0
      local http_reference_case
      for http_reference_case in ${http_reference_cases}; do
        local reference_metadata="${lane_work_dir}/real_reference_existing_${http_reference_case}_metadata.txt"
        [[ -f "${reference_metadata}" ]] || fail "HTTP reference case ${http_reference_case} was not generated; include it in IBMW_JSON_SMOKE_REFERENCE_CASES"
        mapfile -t reference_info < "${reference_metadata}"
        local reference_type_name="${reference_info[1]}"
        local reference_class_name="${reference_info[2]}"
        local reference_snake="${reference_info[3]}"
        local reference_build_dir="${lane_work_dir}/real_reference_existing_${http_reference_case}_generated_project_build"
        [[ -d "${reference_build_dir}" ]] || fail "HTTP reference build dir missing for ${http_reference_case}: ${reference_build_dir}"

        local reference_sub_port=$((http_port + 100 + http_reference_index * 2))
        local reference_pub_port=$((http_port + 100 + http_reference_index * 2 + 1))
        "${py}" - "${reference_build_dir}" "${net_extension}" "${reference_sub_port}" "${reference_pub_port}" "${reference_snake}" "${reference_class_name}" <<'PY'
import pathlib
import sys

build = pathlib.Path(sys.argv[1])
net_extension = sys.argv[2]
sub_port = sys.argv[3]
pub_port = sys.argv[4]
snake = sys.argv[5]
class_name = sys.argv[6]
topic = f"/{snake}"
cfg = build / "cfg"
common = f'''  extensions:
    items:
      - name: net_extension
        path: {net_extension}
        options:
          thread_num: 2
          http_options:
            listen_ip: "127.0.0.1"
            listen_port: {{port}}

  logging:
    core_level: INFO
    backends:
      - type: console

  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool
        options:
          thread_num: 2
'''
sub = 'ibmw:\n' + common.format(port=sub_port) + f'''
  messaging:
    drivers:
      - type: http
        options:
          subscribers:
            - topic: "{topic}"
    publishers: []
    subscribers:
      - topic: "{topic}"
        enable_drivers: [http]

  modules:
    packages:
      - path: lib{snake}_json_subscriber_module_pkg.so
        enable_modules: [{class_name}JsonSubscriberModule]
    modules:
      - name: {class_name}JsonSubscriberModule
        log_level: INFO
'''
pub = 'ibmw:\n' + common.format(port=pub_port) + f'''
  messaging:
    drivers:
      - type: http
        options:
          publishers:
            - topic: "{topic}"
              server_url_list: ["127.0.0.1:{sub_port}"]
    publishers:
      - topic: "{topic}"
        enable_drivers: [http]
    subscribers: []

  modules:
    packages:
      - path: lib{snake}_json_publisher_module_pkg.so
        enable_modules: [{class_name}JsonPublisherModule]
    modules:
      - name: {class_name}JsonPublisherModule
        log_level: INFO
'''
(cfg / f"{snake}_json_http_reference_subscriber_pkg.yaml").write_text(sub, encoding="utf-8")
(cfg / f"{snake}_json_http_reference_publisher_pkg.yaml").write_text(pub, encoding="utf-8")
PY

        local http_reference_sub_log="${lane_work_dir}/real_reference_existing_${http_reference_case}_http_subscriber.log"
        local http_reference_pub_log="${lane_work_dir}/real_reference_existing_${http_reference_case}_http_publisher.log"
        set +e
        (
          cd "${reference_build_dir}"
          timeout 10s "${ibmw_prefix}/bin/ibmw" run "cfg/${reference_snake}_json_http_reference_subscriber_pkg.yaml" > "${http_reference_sub_log}" 2>&1
        ) &
        local reference_sub_pid=$!
        sleep 1
        (
          cd "${reference_build_dir}"
          timeout 5s "${ibmw_prefix}/bin/ibmw" run "cfg/${reference_snake}_json_http_reference_publisher_pkg.yaml" > "${http_reference_pub_log}" 2>&1
        )
        local reference_pub_rc=$?
        wait "${reference_sub_pid}"
        local reference_sub_rc=$?
        set -e
        [[ "${reference_pub_rc}" -eq 0 || "${reference_pub_rc}" -eq 124 ]] || fail "real reference HTTP publisher smoke failed for ${http_reference_case} with rc=${reference_pub_rc}; see ${http_reference_pub_log}"
        [[ "${reference_sub_rc}" -eq 0 || "${reference_sub_rc}" -eq 124 ]] || fail "real reference HTTP subscriber smoke failed for ${http_reference_case} with rc=${reference_sub_rc}; see ${http_reference_sub_log}"
        "${py}" - "${http_reference_case}" "${reference_type_name}" "${http_reference_pub_log}" "${http_reference_sub_log}" <<'PY'
import pathlib
import sys

case = sys.argv[1]
type_name = sys.argv[2]
pub = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8", errors="replace")
sub = pathlib.Path(sys.argv[4]).read_text(encoding="utf-8", errors="replace")
checks = {
    "http_reference_type_registered": f"json:{type_name}" in pub or f"json:{type_name}" in sub,
    "http_reference_published": "Published message #" in pub,
    "http_reference_received": "Received message." in sub,
    "http_reference_driver_registered": "drivers: http" in sub or "http" in sub.lower(),
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    raise SystemExit(f"real reference HTTP 2-process smoke missing evidence for {case}: " + ", ".join(missing))
print(f"[PASS] real reference_existing HTTP 2-process smoke ({case}): {sys.argv[3]} {sys.argv[4]}")
PY
        http_reference_index=$((http_reference_index + 1))
      done
    fi
  fi

  "${py}" - "${lane_work_dir}" "${all_generated_cases}" "${http_cases}" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
all_cases = sys.argv[2].split()
http_selected_cases = sys.argv[3].split()

case_types = {
    "bounded": ("Bounded", "demo_interfaces/msg/Bounded"),
    "dynamic": ("Dynamic", "demo_interfaces/msg/Dynamic"),
    "external_stamped": ("ExternalStamped", "demo_interfaces/msg/ExternalStamped"),
    "fixed_array": ("FixedArray", "demo_interfaces/msg/FixedArray"),
    "nested_detail": ("NestedDetail", "demo_interfaces/msg/NestedDetail"),
    "nested_top": ("NestedTop", "demo_interfaces/msg/NestedTop"),
    "plain": ("Plain", "demo_interfaces/msg/Plain"),
    "scalars": ("Scalars", "demo_interfaces/msg/Scalars"),
}

local_log = work / "json_scaffold_runtime.log"
local_text = local_log.read_text(encoding="utf-8", errors="replace") if local_log.is_file() else ""

def log_contains(path: pathlib.Path, needle: str) -> bool:
    return path.is_file() and needle in path.read_text(encoding="utf-8", errors="replace")

cases = []
for case in all_cases:
    class_name, type_name = case_types[case]
    local_pass = all(
        needle in local_text
        for needle in (
            f"{class_name}JsonPublisherModule::Setup succeeded",
            f"{class_name}JsonSubscriberModule::Setup succeeded",
            f"json:{type_name}",
            "Published message #",
            "Received message.",
        )
    )
    http_requested = case in http_selected_cases
    http_pub_log = work / f"json_scaffold_http_{case}_publisher.log"
    http_sub_log = work / f"json_scaffold_http_{case}_subscriber.log"
    http_pass = bool(
        http_requested
        and log_contains(http_pub_log, "Published message #")
        and log_contains(http_sub_log, "Received message.")
    )
    cases.append(
        {
            "case": case,
            "selected": True,
            "generated": True,
            "status": "pass" if local_pass else "unknown",
            "package": "demo_interfaces",
            "type_name": type_name,
            "local_runtime_log": str(local_log) if local_log.is_file() else None,
            "local_runtime_pass": local_pass,
            "http_requested": http_requested,
            "http_publisher_log": str(http_pub_log) if http_pub_log.is_file() else None,
            "http_subscriber_log": str(http_sub_log) if http_sub_log.is_file() else None,
            "http_runtime_pass": http_pass,
        }
    )

summary = {
    "schema_version": 1,
    "lane": "json_scaffold",
    "artifact": "json_scaffold_generated_shape_matrix_summary",
    "all_alias_cases": all_cases,
    "selected_cases": all_cases,
    "http_selected_cases": http_selected_cases,
    "uses_rcl_in_ibmw_runtime": False,
    "uses_ros2_cli_as_import_prerequisite": False,
    "cases": cases,
    "totals": {
        "selected": len(cases),
        "generated": sum(1 for case in cases if case["generated"]),
        "local_runtime_pass": sum(1 for case in cases if case["local_runtime_pass"]),
        "http_requested": sum(1 for case in cases if case["http_requested"]),
        "http_runtime_pass": sum(1 for case in cases if case["http_runtime_pass"]),
    },
}
path = work / "json_scaffold_generated_shape_matrix_summary.json"
path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[PASS] json scaffold generated shape matrix summary: {path}")
PY

  "${py}" - "${lane_work_dir}" "${all_reference_cases}" "${reference_cases}" "${http_reference_cases}" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
all_cases = sys.argv[2].split()
selected_cases = sys.argv[3].split()
http_selected_cases = sys.argv[4].split()

def read_metadata(case: str):
    path = work / f"real_reference_existing_{case}_metadata.txt"
    if not path.is_file():
        return None
    lines = path.read_text(encoding="utf-8").splitlines()
    return {
        "package": lines[0] if len(lines) > 0 else "",
        "type_name": lines[1] if len(lines) > 1 else "",
        "class_name": lines[2] if len(lines) > 2 else "",
        "snake": lines[3] if len(lines) > 3 else case,
    }

def log_has(path: pathlib.Path, needles):
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8", errors="replace")
    return all(needle in text for needle in needles)

cases = []
for case in selected_cases:
    metadata = read_metadata(case)
    local_log = work / f"real_reference_existing_{case}_json_scaffold_runtime.log"
    http_pub_log = work / f"real_reference_existing_{case}_http_publisher.log"
    http_sub_log = work / f"real_reference_existing_{case}_http_subscriber.log"
    type_name = metadata["type_name"] if metadata else ""
    local_pass = bool(
        metadata
        and log_has(local_log, [f"json:{type_name}", "Published message #", "Received message."])
    )
    http_requested = case in http_selected_cases
    http_pass = bool(
        http_requested
        and metadata
        and http_pub_log.is_file()
        and http_sub_log.is_file()
        and "Published message #" in http_pub_log.read_text(encoding="utf-8", errors="replace")
        and "Received message." in http_sub_log.read_text(encoding="utf-8", errors="replace")
    )
    cases.append(
        {
            "case": case,
            "selected": True,
            "generated": metadata is not None,
            "status": "pass" if local_pass else "skipped" if metadata is None else "unknown",
            "package": metadata["package"] if metadata else None,
            "type_name": type_name or None,
            "local_runtime_log": str(local_log) if local_log.is_file() else None,
            "local_runtime_pass": local_pass,
            "http_requested": http_requested,
            "http_publisher_log": str(http_pub_log) if http_pub_log.is_file() else None,
            "http_subscriber_log": str(http_sub_log) if http_sub_log.is_file() else None,
            "http_runtime_pass": http_pass,
        }
    )

summary = {
    "schema_version": 1,
    "lane": "json_scaffold",
    "artifact": "json_scaffold_reference_matrix_summary",
    "all_alias_cases": all_cases,
    "selected_cases": selected_cases,
    "http_selected_cases": http_selected_cases,
    "uses_rcl_in_ibmw_runtime": False,
    "uses_ros2_cli_as_import_prerequisite": False,
    "cases": cases,
    "totals": {
        "selected": len(selected_cases),
        "generated": sum(1 for case in cases if case["generated"]),
        "local_runtime_pass": sum(1 for case in cases if case["local_runtime_pass"]),
        "http_requested": sum(1 for case in cases if case["http_requested"]),
        "http_runtime_pass": sum(1 for case in cases if case["http_runtime_pass"]),
    },
}
path = work / "json_scaffold_reference_matrix_summary.json"
path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[PASS] json scaffold reference matrix summary: {path}")
PY
}

verify_live_matrix_summary_artifact() {
  local lane_work_dir=$1
  local file_name=$2
  local expected_lane=$3
  local expected_artifact=$4
  local path="${lane_work_dir}/${file_name}"
  [[ -f "${path}" ]] || fail "${expected_lane} summary artifact missing: ${path}"
  ${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${path}" "${expected_lane}" "${expected_artifact}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1]).resolve()
expected_lane = sys.argv[2]
expected_artifact = sys.argv[3]
data = json.loads(path.read_text(encoding="utf-8"))
errors = []

def require(condition, message):
    if not condition:
        errors.append(message)

require(data.get("schema_version") == 1, "schema_version must be 1")
require(data.get("lane") == expected_lane, f"lane must be {expected_lane}")
require(data.get("artifact") == expected_artifact, f"artifact must be {expected_artifact}")
require(data.get("uses_rcl_in_ibmw_runtime") is False, "uses_rcl_in_ibmw_runtime must be false")
require(data.get("uses_ros2_cli_as_import_prerequisite") is False, "uses_ros2_cli_as_import_prerequisite must be false")
cases = data.get("cases") or []
totals = data.get("totals") or {}
require(cases, "cases must not be empty")
require(totals.get("selected") == len(cases), "totals.selected must match cases length")
require(totals.get("pass") == len(cases), "totals.pass must match cases length")
for case in cases:
    require(case.get("status") == "pass", f"case {case.get('case')} status must be pass")
if expected_lane == "action":
    require(data.get("generic_business_semantics_generated") is False, "action summary must keep generic_business_semantics_generated=false")
    for case in cases:
        require(case.get("action_type"), f"action case {case.get('case')} must record action_type")
        require(case.get("action_name"), f"action case {case.get('case')} must record action_name")
        require(case.get("user_hook_binding_contract") is True, f"action case {case.get('case')} must record Hook/Binding contract")

if errors:
    raise SystemExit("\n".join(errors))
print(f"[PASS] {expected_lane} live matrix summary artifact: {path}")
PY
}

verify_json_scaffold_summary_artifacts() {
  local lane_work_dir=$1
  ${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${lane_work_dir}" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1]).resolve()
checks = [
    ("json_scaffold_generated_shape_matrix_summary.json", "json_scaffold_generated_shape_matrix_summary"),
    ("json_scaffold_reference_matrix_summary.json", "json_scaffold_reference_matrix_summary"),
]
errors = []
for file_name, artifact in checks:
    path = work / file_name
    if not path.is_file():
        errors.append(f"json scaffold summary artifact missing: {path}")
        continue
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        errors.append(f"{file_name}: schema_version must be 1")
    if data.get("lane") != "json_scaffold":
        errors.append(f"{file_name}: lane must be json_scaffold")
    if data.get("artifact") != artifact:
        errors.append(f"{file_name}: artifact must be {artifact}")
    if data.get("uses_rcl_in_ibmw_runtime") is not False:
        errors.append(f"{file_name}: uses_rcl_in_ibmw_runtime must be false")
    if data.get("uses_ros2_cli_as_import_prerequisite") is not False:
        errors.append(f"{file_name}: uses_ros2_cli_as_import_prerequisite must be false")
    cases = data.get("cases") or []
    totals = data.get("totals") or {}
    selected = data.get("selected_cases") or []
    http_selected = data.get("http_selected_cases") or []
    if not cases:
        errors.append(f"{file_name}: cases must not be empty")
    if totals.get("selected") != len(selected):
        errors.append(f"{file_name}: totals.selected must match selected_cases length")
    if totals.get("http_requested", 0) != len(http_selected):
        errors.append(f"{file_name}: totals.http_requested must match http_selected_cases length")
    generated = totals.get("generated", 0)
    local_pass = totals.get("local_runtime_pass")
    http_pass = totals.get("http_runtime_pass", 0)
    if artifact == "json_scaffold_generated_shape_matrix_summary":
        if generated < len(selected):
            errors.append(f"{file_name}: totals.generated must cover selected generated-shape cases")
        if local_pass is not None and local_pass < len(selected):
            errors.append(f"{file_name}: local_runtime_pass must cover selected generated-shape cases")
        if http_pass < len(http_selected):
            errors.append(f"{file_name}: http_runtime_pass must cover selected generated-shape HTTP cases")
    else:
        if generated > len(selected):
            errors.append(f"{file_name}: totals.generated cannot exceed selected reference cases")
        if local_pass is not None and local_pass != generated:
            errors.append(f"{file_name}: local_runtime_pass must match generated reference cases")
        generated_http_cases = [case for case in cases if case.get("http_requested") and case.get("generated")]
        if http_pass != len(generated_http_cases):
            errors.append(f"{file_name}: http_runtime_pass must match generated requested reference HTTP cases")
if errors:
    raise SystemExit("\n".join(errors))
for file_name, _ in checks:
    print(f"[PASS] json scaffold summary artifact: {work / file_name}")
PY
}

verify_route_b_target_artifact_audit() {
  local audit_json="${ROUTE_B_BUILD_DIR}/logs/route_b_target_artifact_audit.json"
  [[ -f "${audit_json}" ]] || fail "route_b target artifact audit missing: ${audit_json}"
  ${PYTHON_EXECUTABLE:-/usr/bin/python3} - "${audit_json}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1]).resolve()
data = json.loads(path.read_text(encoding="utf-8"))
errors = []
if data.get("status") != "pass":
    errors.append(f"status must be pass, got {data.get('status')}")
if data.get("expected_arch") != "aarch64":
    errors.append(f"expected_arch must be aarch64, got {data.get('expected_arch')}")
artifacts = data.get("artifacts") or []
if not artifacts:
    errors.append("artifacts must not be empty")
for artifact in artifacts:
    if artifact.get("status") != "pass":
        errors.append(f"artifact {artifact.get('path')} status must be pass")
    if artifact.get("arch") != "aarch64":
        errors.append(f"artifact {artifact.get('path')} arch must be aarch64")
    if artifact.get("forbidden_prefix_hits"):
        errors.append(f"artifact {artifact.get('path')} must not contain forbidden prefix hits")
if errors:
    raise SystemExit("\n".join(errors))
print(f"[PASS] route_b target artifact audit: {path}")
PY
}

run_lane() {
  local lane=$1
  local lane_work_dir="${WORK_ROOT}/${lane}"
  echo "=========================================="
  echo "ROS2 interface import live verification lane: ${lane}"
  echo "Work dir: ${lane_work_dir}"
  echo "=========================================="
  case "${lane}" in
    msg)
      run_with_lane_timeout "${lane}" \
        env WORK_ROOT="${lane_work_dir}" \
        bash "${ROOT_DIR}/scripts/demo_ros2_native_msg_matrix.sh"
      verify_live_matrix_summary_artifact "${lane_work_dir}" "msg_live_matrix_summary.json" "msg" "msg_live_matrix_summary"
      ;;
    imported_srv)
      run_with_lane_timeout "${lane}" \
        bash "${ROOT_DIR}/scripts/live_ros2_imported_srv_acceptance.sh" "${lane_work_dir}"
      verify_live_matrix_summary_artifact "${lane_work_dir}" "imported_srv_live_matrix_summary.json" "imported_srv" "imported_srv_live_matrix_summary"
      ;;
    reference_srv)
      run_with_lane_timeout "${lane}" \
        bash "${ROOT_DIR}/scripts/live_ros2_reference_srv_acceptance.sh" "${lane_work_dir}"
      verify_live_matrix_summary_artifact "${lane_work_dir}" "reference_srv_live_matrix_summary.json" "reference_srv" "reference_srv_live_matrix_summary"
      ;;
    action)
      run_with_lane_timeout "${lane}" \
        env RUN_CASES="${ACTION_RUN_CASES}" \
        bash "${ROOT_DIR}/scripts/live_ros2_generated_action_matrix.sh" "${lane_work_dir}"
      verify_live_matrix_summary_artifact "${lane_work_dir}" "action_runtime_matrix_summary.json" "action" "action_runtime_matrix_summary"
      ;;
    route_b)
      run_with_lane_timeout "${lane}" \
        bash "${ROOT_DIR}/scripts/check_ohos_ros2_rosidl_compile.sh" "${ROUTE_B_BUILD_DIR}"
      verify_route_b_target_artifact_audit
      ;;
    backend_metadata)
      verify_backend_metadata
      ;;
    perf_boundary)
      verify_perf_boundary "${lane_work_dir}"
      ;;
    json_scaffold)
      verify_json_scaffold_manifest
      verify_json_scaffold_runtime "${lane_work_dir}"
      verify_json_scaffold_summary_artifacts "${lane_work_dir}"
      ;;
    *)
      fail "unsupported VERIFY_LANES entry: ${lane}"
      ;;
  esac
  echo "[PASS] ROS2 interface import live verification lane passed: ${lane}"
}

for lane in ${VERIFY_LANES}; do
  run_lane "${lane}"
done

echo "[PASS] ROS2 interface import live verification completed: ${VERIFY_LANES}"
