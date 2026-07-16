# ROS2 Interface Import User Guide

This guide explains how to migrate ROS2 interface contracts into IBMW with
`ibmw-gen import`. It focuses on copyable commands, generated artifacts,
verification, and current boundaries.

For application-level migration from native ROS2 source into DDS/ROS2-compatible
and JSON/local application roles, use the
standalone manual in [`skills/migration/SKILL.md`](../../skills/migration/SKILL.md). That skill states exactly what
users must rewrite, what can be generated, which runtime checks must pass, and
when OHOS / Route B target artifact audit is required before claiming completion.

The core rule is:

```text
If an installed ROS2 package/type already exists, prefer reference_existing.
If only source .msg/.srv/.action files exist, use generate_package/source mode.
```

IBMW generated runtime must remain **rcl-free**: it must not link or depend on
`rcl`, `rclcpp`, or `rclpy`. ROS2 CLI, `rclpy`, and `rclcpp` are only used by
external live verification peers.

---

## 1. Choose the migration path

| Input you have | Recommended path | Why |
|----------------|------------------|-----|
| Installed ROS2 package/type, e.g. `std_msgs/msg/String` | `reference_existing` | Reuses upstream rosidl generated targets; avoids copying interface files. |
| ROS2 source package in a workspace, not installed yet | `generate_package` via `--ros2-workspace` | Uses the source package directly and emits a rosidl artifact package. |
| Loose package-shaped directory with `.msg/.srv/.action` | `generate_package` via `--ros2-src` | Creates a minimal rosidl artifact package. |
| You only want to discover available types | `--list-ros2-interfaces` | Produces a deterministic report without changing build behavior. |

Do not silently mix existing installed types and local source definitions in the
same dependency closure. Choose one path explicitly.

---

## 2. Discover installed ROS2 interfaces

List all installed interface packages/types under a ROS2 prefix:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-prefix /opt/ros/humble \
  --list-ros2-interfaces \
  --format json
```

List one package:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-prefix /opt/ros/humble \
  --ros2-package sensor_msgs \
  --list-ros2-interfaces \
  --format json
```

The report includes:

- `mode=reference_existing_discovery`
- package count and type count
- `available_types` grouped by `msg`, `srv`, and `action`
- `uses_ros2_cli=false`
- `uses_ament_index_python=false`

Discovery is a host-side file scan of `<prefix>/share/<pkg>`. The prefix is not
treated as a target runtime/sysroot path.

---

## 3. Import an installed package/type: `reference_existing`

Use this when the ROS2 package/type already exists in a ROS2 install prefix:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-prefix /opt/ros/humble \
  --ros2-package std_msgs \
  --ros2-type msg/String \
  --artifact-only \
  --output-dir /tmp/std_msgs_string_import \
  --format json
```

If `--ros2-type` is omitted, the package-level report records all available
types. If a requested type is missing or ambiguous, the error lists available
selectors such as `msg/String`, `srv/SetBool`, or `action/Fibonacci`.

Generated artifacts can include:

| Artifact | Meaning |
|----------|---------|
| `ibmw_interface_report.json` | Top-level deterministic import report. |
| `<pkg>_dependency_closure.json` | Selected interfaces and transitive dependency closure. |
| `<pkg>_migration_manifest.json` | Recommended mode, selected types, verification plan, backend status. |
| `include/<pkg>/ibmw_interface_types.hpp` | IBMW-facing type aliases/wrappers. |
| `include/<pkg>/ibmw_ros2_conversion.hpp` | ROSIDL identity/copy conversion helpers. |
| `<pkg>_reference_existing_srv_runtime.ibmw.yaml` | Optional `.srv` runtime scaffold when services are selected. |

Important boundary:

- `reference_existing` reuses existing rosidl targets with `find_package(<pkg>)`.
- It does not copy or regenerate upstream `.msg/.srv/.action` files.
- The generated IBMW runtime path remains rcl-free.

---

## 4. Import source/workspace interfaces: `generate_package`

### 4.1 Package-shaped source directory

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-src /tmp/my_interfaces \
  --artifact-only \
  --package-name my_interfaces \
  --output-dir /tmp/my_interfaces_gen \
  --target dds_ros2 \
  --format json
```

The output is a minimal rosidl artifact package plus IBMW handoff artifacts.

### 4.2 Workspace source package shortcut

Discover packages in a workspace:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-workspace "<ros2-workspace>" \
  --list-ros2-interfaces \
  --format json
```

Import one source package from that workspace:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-workspace "<ros2-workspace>" \
  --ros2-package my_interfaces \
  --artifact-only \
  --output-dir /tmp/my_interfaces_gen \
  --format json
```

This is still `generate_package`, not `reference_existing`: it uses source files
from the workspace and emits a new artifact package.

---

## 5. Imported `.srv` runtime scaffold

When a source-mode or reference-existing import includes `.srv` files, `ibmw-gen`
can emit a runtime project skeleton YAML.

Example source-mode `.srv` flow:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-src /tmp/set_mode_interfaces \
  --artifact-only \
  --package-name set_mode_interfaces \
  --output-dir /tmp/set_mode_interfaces_gen \
  --target dds_ros2

python3 src/tools/ibmw_gen/ibmw_gen.py \
  /tmp/set_mode_interfaces_gen/set_mode_interfaces_imported_srv_runtime.ibmw.yaml \
  -o /tmp/set_mode_runtime --force
```

Generated server/client modules contain user regions for business logic. The
generator provides typed request/response glue; it does not infer arbitrary
service semantics.

Live acceptance scripts:

```bash
bash scripts/live_ros2_imported_srv_acceptance.sh
bash scripts/live_ros2_reference_srv_acceptance.sh
```

---

## 6. JSON scaffold workflow

Use `--target json` to emit JSON handoff artifacts for plain-compatible `.msg`
files:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-src /tmp/demo_interfaces \
  --artifact-only \
  --package-name demo_interfaces \
  --output-dir /tmp/demo_interfaces_gen \
  --target json

python3 src/tools/ibmw_gen/ibmw_gen.py \
  /tmp/demo_interfaces_gen/demo_interfaces_json_runtime_scaffold.ibmw.yaml \
  -o /tmp/demo_json_runtime --force

cmake -S /tmp/demo_json_runtime -B /tmp/demo_json_runtime_build \
  -DCMAKE_PREFIX_PATH="$IBMW_INSTALL_PREFIX;/opt/ros/humble" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/demo_json_runtime_build -j$(nproc)
```

Important artifacts:

| Artifact | Meaning |
|----------|---------|
| `<pkg>_json_types.hpp` | C++ JSON structs with `kTypeName` and nlohmann JSON macros. |
| `<pkg>_json_runtime_scaffold.ibmw.yaml` | Second-stage IBMW JSON pub/sub project scaffold. |
| `<pkg>_json_backend_metadata.json` | JSON backend status and type artifact metadata. |
| `json_scaffold_generated_shape_matrix_summary.json` | Verification-only summary emitted by the `json_scaffold` lane; records the built-in generated shape matrix, local runtime evidence, and optional HTTP evidence. |
| `json_scaffold_reference_matrix_summary.json` | Verification-only summary emitted by the `json_scaffold` lane; records selected real reference cases, generated/skipped status, local runtime evidence, and optional HTTP evidence. |

Verified shape coverage currently includes primitive scalar fields, fixed
arrays, strings, bounded/unbounded sequences, same-package nested plain message
fields, source/workspace external-package nested message fields, and reference-
existing external nested type/scaffold bundles. The unified `json_scaffold` lane
also compiles and runs a reference-existing `std_msgs/msg/Header ->
builtin_interfaces/msg/Time` local pub/sub smoke, and conditionally runs a real
installed-prefix `/opt/ros/humble` reference matrix when those packages exist.
Set `IBMW_JSON_SMOKE_REFERENCE_CASES=all` to cover twenty-three common installed
interfaces: `pose_stamped`, `odometry`, `imu`, `transform_stamped`, `twist`,
`laser_scan`, `path`, `point_cloud2`, `nav_sat_fix`, and
`pose_with_covariance_stamped`, plus `point_stamped`, `vector3_stamped`,
`wrench_stamped`, `accel_stamped`, `image`, `camera_info`, `joint_state`,
`battery_state`, `range`, `fluid_pressure`, `temperature`, `occupancy_grid`, and
`map_meta_data`. Optional HTTP two-process smoke covers the generated shape
matrix, but JSON is still reported as `degraded`, not fully `supported`, until
broader runtime lowering is complete. This matrix expansion is verification-only
and does not add runtime hot-path work.

---

## 7. `.action` runtime scaffold boundary

Action import emits metadata, traits, skeletons, conversion helpers, and a
rcl-free server runtime template. The generated runtime template is designed for
user-owned `Hook` and `ActionServerBinding` code.

Current verified live matrix covers several installed actions through generated
runtime template + user-owned Hook/Binding:

- `example_interfaces/action/Fibonacci`
- `control_msgs/action/GripperCommand`
- `nav2_msgs/action/NavigateToPose`
- `nav2_msgs/action/FollowPath`
- `nav2_msgs/action/NavigateThroughPoses`
- `moveit_msgs/action/ExecuteTrajectory`
- `moveit_msgs/action/MoveGroup`

Run the action matrix:

```bash
bash scripts/live_ros2_generated_action_matrix.sh /tmp/ibmw_action_verify

RUN_CASES="fibonacci move_group" \
bash scripts/live_ros2_generated_action_matrix.sh /tmp/ibmw_action_verify_subset
```

Boundary:

- The runtime server path is rcl-free.
- Native ROS2 action clients are external verification peers only.
- IBMW does not auto-generate arbitrary action business semantics; users own the
  Hook/Binding behavior.

See also: [ROS2 Action DDS-only Adapter Guide](../experimental/ros2-action-dds-only-adapter-guide.md).

---

## 8. Migration manifest and verification

Each import emits `<pkg>_migration_manifest.json` when enough context is
available. It is the handoff file for users and CI.

Important fields:

| Field | Meaning |
|-------|---------|
| `recommended_mode` | `reference_existing` or `generate_package`. |
| `selected` | Selected canonical interfaces and selectors. |
| `dependency_closure_status` | `complete` or `incomplete`. |
| `target_backend_status` | Backend status summary and per-backend entries. |
| `backend_metadata_files` | Metadata sidecars for degraded/non-DDS backends. |
| `backend_generated_files` | Generated non-DDS artifacts such as JSON types/scaffolds. |
| `verification.entrypoint` | Usually `scripts/verify_ros2_interface_import_live.sh`. |
| `verification.selected_lanes` | Default verification lanes for this manifest. |
| `verification.optional_lanes` | Optional lanes such as `route_b`. |

Run manifest-selected verification:

```bash
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json \
  /tmp/ibmw_manifest_verify
```

Override lanes manually:

```bash
VERIFY_LANES="backend_metadata json_scaffold" \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json \
  /tmp/ibmw_manifest_verify
```

When `json_scaffold` runs with `--manifest`, the wrapper first checks that
`backend_generated_files` lists both `<pkg>_json_types.hpp` and
`<pkg>_json_runtime_scaffold.ibmw.yaml`, and that those files exist next to the
manifest. This catches stale or hand-edited manifests before the generic JSON
smoke starts.

When `perf_boundary` runs with `--manifest`, the wrapper performs a CI-friendly
guard over backend status and metadata sidecars. It does not change runtime code
or benchmark the hot path; it verifies that `dds_ros2` remains explicitly
rcl-free, that `dds_native` / `iceoryx` are not promoted by metadata-only
artifacts, and that zero-copy candidates carry deterministic eligibility
classification before any future lowering can claim support.
Migration manifests now select this lane automatically when `dds_native` or
`iceoryx` backend metadata sidecars are emitted.
The emitted `perf_boundary_summary.json` also reports
`zero_copy_sized_interfaces`, counting `iceoryx`-eligible interfaces whose
metadata contains an IR-derived `max_serialized_size_bytes` bound.
When `iceoryx` is requested, the import also emits
`<pkg>_iceoryx_lowering_plan.json`. This plan is metadata-only: it lists
eligible interfaces, blocked interfaces, size-bound semantics, future generated
type/module names, and the required compile/runtime promotion gate without
generating runtime lowering.
When `dds_native` is requested, the import emits
`<pkg>_dds_native_lowering_plan.json` with eligible/degraded interfaces, future
IDL/schema/type artifact names, size-bound semantics, and the required
schema/IDL compile-smoke then runtime promotion gate. It is also plan-only and
keeps `runtime_lowering_generated=false`.

The lane also writes `json_scaffold_generated_shape_matrix_summary.json` for the
built-in generated shape matrix. When the real reference matrix runs, it writes
`json_scaffold_reference_matrix_summary.json` under the lane work directory. The
summaries are deterministic JSON and are intended for CI artifacts or quick
manual audit of selected/generated cases, local runtime passes, and optional
HTTP passes without reading the full build log.
The unified wrapper now validates both JSON summary artifacts after the lane
finishes, including schema/lane names, rcl-free fields, selected totals, and
generated/local pass consistency. HTTP-selected cases are also checked against
`http_requested` and `http_runtime_pass` totals so two-process evidence cannot
silently disappear. Real-prefix reference cases may remain skipped when the
corresponding ROS2 prefix/type is unavailable.

The `route_b` lane also treats
`logs/route_b_target_artifact_audit.json` as a first-class contract: after the
cross compile helper returns, the wrapper verifies `status=pass`,
`expected_arch=aarch64`, at least one audited ELF, per-artifact pass status,
AArch64 architecture, and no forbidden host-prefix hits.

For CI lane splitting, the wrapper accepts optional timeout caps for delegated
lane scripts without changing local defaults. Use
`IBMW_VERIFY_LANE_TIMEOUT_SECONDS=<seconds>` as a default cap, or a per-lane
override such as `IBMW_VERIFY_ROUTE_B_TIMEOUT_SECONDS=1800`. The `json_scaffold`
lane keeps its existing internal command-level timeouts.

---

## 9. Verification lanes

`scripts/verify_ros2_interface_import_live.sh` is the unified verification
entrypoint.

| Lane | What it verifies |
|------|------------------|
| `msg` | 40-case ROS2 native `.msg` matrix. |
| `imported_srv` | Source-mode custom `.srv` live acceptance. |
| `reference_srv` | Installed/reference-existing `.srv` live acceptance. |
| `action` | Generated action runtime template live matrix. |
| `route_b` | OHOS/Route B target compile and target artifact audit. |
| `backend_metadata` | Manifest sidecar/generated-artifact consistency. |
| `perf_boundary` | Manifest/backend-metadata guard for rcl-free and zero-copy promotion boundaries; writes `perf_boundary_summary.json`. |
| `json_scaffold` | JSON scaffold compile and same-process local pub/sub smoke; optional HTTP two-process pub/sub smoke. |

The live `.srv` and `.action` lanes now also emit deterministic matrix summaries
when they complete successfully:

- `msg_live_matrix_summary.json`
- `imported_srv_live_matrix_summary.json`
- `reference_srv_live_matrix_summary.json`
- `action_runtime_matrix_summary.json`

These artifacts record selected cases, type/topic metadata, pass totals,
rcl-free boundaries, and the action Hook/Binding business-semantics boundary
without replacing the full logs.
The unified wrapper now treats these summaries as lane contracts: after
`msg`, `imported_srv`, `reference_srv`, or `action` completes, it checks that the
expected summary file exists, all selected cases pass, rcl-free fields remain
false/true as appropriate, and the action summary still records that generic
business semantics are not auto-generated. Action cases also record stable
`action_type` and `action_name` fields for CI dashboards and coverage audits.

Useful environment variables:

| Variable | Used by | Meaning |
|----------|---------|---------|
| `VERIFY_LANES` | wrapper | Space-separated lane list. |
| `ACTION_RUN_CASES` | `action` | Subset of action cases to run. |
| `ROUTE_B_BUILD_DIR` | `route_b` | Build dir for Route B compile/audit. |
| `IBMW_JSON_SMOKE_IBMW_PREFIX` | `json_scaffold` | Existing installed IBMW prefix to reuse; when unset, the lane builds one and enables net extension automatically for HTTP smoke. |
| `IBMW_JSON_SMOKE_EXTRA_PREFIX` | `json_scaffold` | Extra CMake prefix, defaults to `/opt/ros/humble` when present. |
| `IBMW_JSON_SMOKE_HTTP_2PROC` | `json_scaffold` | Set to `1` to also run selected cases through HTTP publisher and subscriber processes. |
| `IBMW_JSON_SMOKE_NET_EXTENSION` | `json_scaffold` | Optional path to `libibmw_net_transport.so`; otherwise the lane probes the IBMW prefix. |
| `IBMW_JSON_SMOKE_HTTP_CASES` | `json_scaffold` | Space-separated HTTP cases or `all`. Default: `plain`; current matrix: `plain scalars fixed_array bounded dynamic nested_detail nested_top external_stamped`. |
| `IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES` | `json_scaffold` | Space-separated real installed-prefix reference cases for HTTP smoke or `all`. Default: empty; requested cases must also be selected by `IBMW_JSON_SMOKE_REFERENCE_CASES`. |

Examples:

```bash
VERIFY_LANES=backend_metadata \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json

VERIFY_LANES=perf_boundary \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json

IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_INSTALL_PREFIX" \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_verify

IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_NET_INSTALL_PREFIX" \
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_CASES=all \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_verify

IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_CASES="plain bounded" \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_verify_build_prefix

IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_NET_INSTALL_PREFIX" \
IBMW_JSON_SMOKE_REFERENCE_CASES=all \
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES=all \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_reference_verify

ACTION_RUN_CASES="execute_trajectory move_group" \
VERIFY_LANES=action \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_action_verify
```

---

## 10. Backend status semantics

`target_backend_status.summary` is intentionally conservative:

| Status | Meaning |
|--------|---------|
| `supported` | The backend has generated artifacts and matching compile/live evidence for the claimed scope. |
| `degraded` | Some metadata/type/scaffold artifacts exist, but runtime lowering or full coverage is incomplete. |
| `blocked` | The interface shape cannot be mapped yet, or required inputs are missing. |

Current status boundaries:

- `dds_ros2`: supported for the verified ROS2-compatible DDS lanes and rcl-free
  runtime boundary.
- `json`: degraded; type artifact + scaffold + local pub/sub smoke exist, but
  full runtime lowering and broader shape/transport coverage are not complete.
- `dds_native`: degraded metadata sidecar only.
- `iceoryx`: degraded metadata sidecar only.

Do not treat a degraded backend as production-ready runtime support. Zero-copy
status changes require a separate promotion path: plain/fixed-size eligibility,
no string/unbounded/dynamic heap promoted path, no new hot-path memcpy or
serialization fallback, and backend-specific compile/runtime evidence. If a path
still copies or serializes, it must remain non-zero-copy/degraded.
`max_serialized_size_bytes` is a deterministic packed ROS2-IR size bound for
metadata and CI guards; it is not C++ `sizeof(T)` and must not be wired into
runtime chunk sizing without a backend-specific compile/runtime check.
For resolved nested plain `.msg` dependencies, this bound now rolls up nested
primitive/fixed-array field sizes into the parent interface and into the
`dds_native` / `iceoryx` lowering plans.

---

## 11. Long-cycle TODO alignment

The long-cycle target remains ROS2 interface import plus multi-transport
generation, not a ROS2-compatible DDS-only path. Keep the following unfinished
items visible when extending import, manifests, or verification:

1. **JSON scaffold to runtime lane**
   - Keep `json` reported as `degraded` until generated runtime lowering covers
     representative ROS2 message shapes beyond handoff scaffolds.
   - Keep expanding the opt-in HTTP two-process smoke matrix beyond the current
     generated shapes (`plain`, `scalars`, `fixed_array`, `bounded`, `dynamic`,
     `nested_detail`, `nested_top`, `external_stamped`) as new JSON shapes are
     lowered.
   - Keep the auto-built HTTP prefix path healthy; when no prefix is supplied,
     the lane must build IBMW with `IBMW_BUILD_NET_EXTENSION=ON`.

2. **Reference-existing runtime coverage**
   - Keep the reference-existing external nested JSON runtime smoke healthy for
     `std_msgs/msg/Header -> builtin_interfaces/msg/Time`.
   - Keep the real installed-prefix matrix healthy when `/opt/ros/humble` is
     available: `pose_stamped`, `odometry`, `imu`, `transform_stamped`, `twist`,
     `laser_scan`, `path`, `point_cloud2`, `nav_sat_fix`, and
     `pose_with_covariance_stamped`, plus `point_stamped`, `vector3_stamped`,
     `wrench_stamped`, `accel_stamped`, `image`, `camera_info`, `joint_state`,
     `battery_state`, `range`, `fluid_pressure`, `temperature`,
     `occupancy_grid`, and `map_meta_data` via
     `IBMW_JSON_SMOKE_REFERENCE_CASES=all`.
   - Keep the opt-in reference HTTP smoke healthy for the same cases via
     `IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES=all`; this proves two-process HTTP
     handoff for installed-prefix scaffolds but still does not promote `json`
     to fully supported.
   - Preserve the `reference_existing`-first migration rule for standard ROS2
     packages and only fall back to `generate_package` when no existing type is
     available.

3. **Action semantics boundary**
   - Keep generated action runtime templates rcl-free.
   - Do not claim arbitrary business semantics are generated; user-owned
     `Hook`/`Binding` remains the contract until semantic generators exist.

4. **Additional backend lowering**
   - Advance `dds_native` and `iceoryx` from metadata/degraded handoff status to
      deterministic generated artifacts, compile smoke, and runtime gates only
      after the `perf_boundary`/zero-copy promotion guard is green.
   - Keep backend status manifests conservative and evidence-driven.

5. **CI and reproducibility**
   - Split `.msg`, `.srv`, `.action`, `backend_metadata`, `json_scaffold`, HTTP
     JSON, and Route B gates into cacheable jobs with explicit timeouts.
   - Keep `scripts/verify_ros2_interface_import_live.sh --manifest` as the
     user-facing reproducible entrypoint.

6. **Optional coverage expansion queue**
   - Extend real-prefix reference `.msg` evidence beyond the current 23-case
     matrix only when each new case has deterministic local/HTTP summary
     evidence.
   - Add more Nav2/MoveIt-style action cases as runtime-template evidence while
     preserving the user-owned Hook/Binding business-semantics boundary.
   - Broaden JSON non-local coverage beyond the current HTTP opt-in matrix only
     with explicit transport/case selection and summary artifacts.
   - Keep nested plain size-bound metadata healthy for `dds_native` and
     `iceoryx`; do not use packed IR bounds as runtime `sizeof(T)`.

---

## 12. Documentation/update rule for contributors

Every new ROS2 import feature should update user documentation in the same slice
as the code change. Before closing a feature, check:

1. Is there a copyable command for the new path?
2. Are generated artifacts named and explained?
3. Is there a verification command or lane?
4. Are supported shapes and unsupported boundaries explicit?
5. Does the manifest/backend status text match the implementation?

If any answer is no, the feature is not user-ready.
