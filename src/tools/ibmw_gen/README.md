# ibmw-gen -- IBMW Project Code Generator

`ibmw-gen` generates a complete, buildable IBMW project from a single
`.ibmw.yaml` definition file. It produces:

- **Module scaffolds** (`.h` + `.cc`) with lifecycle hooks and TODO markers
- **App entry points** (`main.cc`) for standalone executables
- **Pkg loaders** (`pkg_main.cc`) for `ibmw run` (shared library mode)
- **YAML configurations** for both App mode and Pkg mode
- **CMakeLists.txt** with correct target linking and ROS2/DDS integration
- **Custom DDS type headers** (C++ structs with inline FastCDR serialization)
- **JSON type definitions** (if configured)

For ROS2 interface migration workflows (`ibmw-gen import`), see the
[ROS2 Interface Import User Guide](../../../docs/guides/ros2-interface-import-user-guide.md).

## Quick Start

```bash
export IBMW_GEN="<ibmw-source-dir>/src/tools/ibmw_gen/ibmw_gen.py"
export IBMW_BUILD_DIR="<ibmw-build-dir>"

# 1. Write a .ibmw.yaml (see examples below)

# 2. Generate
python3 "$IBMW_GEN" my_project.ibmw.yaml -o my_project_gen

# 3. Build
mkdir -p my_project_gen/build && cd my_project_gen/build
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 4. Run (App mode)
./my_app cfg/my_app.yaml

# 5. Run (Pkg mode)
ibmw run cfg/my_app_pkg.yaml
```

## Usage

```
ibmw-gen [-h] [-o OUTPUT_DIR] [--force] [--dry-run] [--only CATEGORIES] [-v] [--version] yaml_file
```

| Flag | Description |
|------|-------------|
| `yaml_file` | Path to `.ibmw.yaml` project definition |
| `-o`, `--output-dir` | Output directory (default: `./<project.name>/`) |
| `--force` | Overwrite ALL files including user-edited modules |
| `--dry-run` | Print what would be generated without writing |
| `--only` | Generate specific categories: `types,modules,apps,cmake,configs,schema,actions,all` |
| `-v`, `--verbose` | Verbose output |
| `--version` | Show version |

## What Gets Generated vs What You Write

This is the key concept of ibmw-gen: **the tool generates the boilerplate; you
write the business logic.**

### Auto-generated (overwritten on re-run)

| File | Description |
|------|-------------|
| `CMakeLists.txt` | Build system with all targets and dependencies |
| `cfg/*.yaml` | App-mode and Pkg-mode YAML configs |
| `src/<app>/main.cc` | App-mode entry points (init framework + run) |
| `src/<app>_pkg/pkg_main.cc` | Pkg-mode shared library loader (one per app) |
| `protocol/<message>.h` | Custom DDS type headers (struct + inline CDR) |
| `src/json_custom_types.h` | JSON type definitions |
| `src/experimental_actions/` | Private experimental DDS-only Fibonacci Action proof sources |

### Generated once, then yours to edit (skipped on re-run)

| File | Description |
|------|-------------|
| `src/<module>/<module>.h` | Module header with class declaration |
| `src/<module>/<module>.cc` | Module implementation with TODO markers |

These files contain `// TODO(ibmw-gen)` markers where you add your business
logic. On subsequent `ibmw-gen` runs, they are **skipped** to preserve your
edits. Use `--force` to overwrite them.

## ROS2-Compatible DDS-Only Usability Matrix

`ibmw-gen` has three ROS2-compatible DDS-only lanes today. They share the same
goal: keep the generated IBMW side on direct DDS/FastDDS wiring without adding
`rcl`, `rclcpp`, `rcl_action`, `rclcpp_action`, or `rmw` runtime dependencies.

| ROS2 shape | Current ibmw-gen lane | Example | Status | Boundary |
|------------|-----------------------|---------|--------|----------|
| MSG pub/sub | `messages.<cpp_type>.wire.ros2` + `direct_ros2_publisher` / `direct_ros2_subscriber` | `src/samples/cpp/ros2_interop_demo/` | Supported for generated DDS-compatible pub/sub scaffolds | Uses DDS extension with `ros2_compatible_mode: true`; ROS2 peer/runtime stays outside IBMW |
| SRV request/response | `dds_types.rpc_services[].srv` + `rpc_server` / `rpc_client` over `transport: dds` | `src/samples/cpp/dds_rpc_demo/` | Supported for generated DDS RPC projects with ROS2-compatible service/type-name derivation | Pure DDS service driver; no `rcl` / `rclcpp` server/client in generated IBMW binaries |
| Action goal/feedback/result | `experimental_actions` | `src/samples/cpp/ros2_action_fibonacci_gen/` | Experimental Fibonacci-only compile/link sample | Only `example_interfaces/action/Fibonacci`, only `transport: dds`, no public `action_server` / `action_client` module types |

This matrix is intentionally conservative. Generic `.action` parsing and a
public Action runtime are not claimed yet; the current Action lane only proves
the experimental Fibonacci DDS-only slice can be generated and linked without ROS2
client-library runtime dependencies.

The metadata layer beneath that lane (action name derivation and FastDDS
type-support header rendering) is action-agnostic and is unit-tested against a
second action (`nav2_msgs/action/NavigateToPose`) in
`tests/test_experimental_actions.py`. This is a generator-internal guarantee
only: the public `experimental_actions` schema, CLI acceptance, CMake emission,
and the compiled/linked sample all remain Fibonacci-only.

## ROS2 Interface Import: Imported `.srv` Runtime Workflow

`ibmw-gen import --ros2-src ... --artifact-only` can turn a source-mode ROS2
interface package into a rosidl-first artifact package. When the input contains
`.srv` files, the generated artifact package also includes an optional
`<package>_imported_srv_runtime.ibmw.yaml` skeleton. Feed that skeleton back to
`ibmw-gen` to create IBMW server/client modules that use the imported ROS2
service type support.

Minimal flow:

```bash
export IBMW_GEN="<ibmw-source-dir>/src/tools/ibmw_gen/ibmw_gen.py"

# 1. Prepare a ROS2 interface package or package-shaped directory.
mkdir -p /tmp/set_mode_interfaces/srv
cat > /tmp/set_mode_interfaces/srv/SetMode.srv <<'EOF'
string mode
bool enable
---
bool accepted
string reason
EOF

# 2. Generate the rosidl artifact package plus the optional IBMW runtime YAML.
python3 "$IBMW_GEN" import \
  --ros2-src /tmp/set_mode_interfaces \
  --artifact-only \
  --package-name set_mode_interfaces \
  --output-dir /tmp/set_mode_interfaces_gen \
  --target dds_ros2

# 3. Build/install the artifact package with ROS2/colcon so C++ service types
#    and rosidl_typesupport_cpp targets are available to the IBMW runtime project.
mkdir -p /tmp/ws/src
cp -a /tmp/set_mode_interfaces_gen /tmp/ws/src/set_mode_interfaces
colcon build --base-paths /tmp/ws/src --merge-install

# 4. Generate the IBMW runtime project from the emitted skeleton YAML.
python3 "$IBMW_GEN" \
  /tmp/set_mode_interfaces_gen/set_mode_interfaces_imported_srv_runtime.ibmw.yaml \
  -o /tmp/set_mode_runtime --force
```

Generated runtime modules contain user-region markers for business code:

```cpp
// Server user region: set the response and return an RPC status.
// IBMW_RPC_SERVER_HANDLER_BODY_SET_MODE_BEGIN
rsp.accepted = req.enable && req.mode == "auto";
rsp.reason = rsp.accepted ? "accepted:auto" : "rejected";
return ibmw::rpc::Status();
// IBMW_RPC_SERVER_HANDLER_BODY_SET_MODE_END

// Client user region: populate the request before the generated call.
// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_MODE_BEGIN
req.mode = "auto";
req.enable = true;
// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_MODE_END

// Client user region: consume/validate the response after a successful call.
// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_MODE_BEGIN
MW_INFO("SetMode response accepted={}, reason={}", rsp.accepted, rsp.reason);
// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_MODE_END
```

Important behavior and boundaries:

- The generated runtime YAML sets `ros2_imported_srv_adapter: true` and
  `ros2_service_name: /<snake_service>` for both server and client modules.
- Generated app configs remap the canonical IBMW function name
  `ros2:/<package>/srv/<Service>` to the friendly ROS2 service name, e.g.
  `/set_mode`. Stock ROS2 CLI/users should call the friendly name.
- Server/client adapters are typed and use the imported package's
  `__rosidl_typesupport_cpp` target plus `ibmw::GetRos2MessageTypeSupport<T>()`.
- The artifact package itself is still a rosidl package; the runtime adapter
  header is for an IBMW-aware consumer target and requires C++20 plus the IBMW
  public API and ROS2 type support dependencies.
- The generator preserves business logic by keeping request/response handling in
  user regions; it does not infer arbitrary service semantics.

Live acceptance is covered by:

```bash
bash scripts/live_ros2_imported_srv_acceptance.sh
```

That script proves both directions for a custom source-mode `SetMode.srv` over
the friendly `/set_mode` service name:

- stock ROS2 CLI -> generated IBMW server
- generated IBMW client -> stock ROS2 Python server

## ROS2 Interface Import: JSON Scaffold Workflow

`ibmw-gen import --target json` can also emit a JSON handoff scaffold for
plain-compatible ROS2 `.msg` files. This path is intended for migration teams
that want to start from existing ROS2 message contracts, generate C++ JSON
structs, and then create normal IBMW JSON publisher/subscriber modules from the
emitted scaffold YAML.

Minimal flow:

```bash
export IBMW_GEN="<ibmw-source-dir>/src/tools/ibmw_gen/ibmw_gen.py"
export IBMW_INSTALL_PREFIX="<ibmw-install-prefix>"
export IBMW_NET_INSTALL_PREFIX="<net-enabled-ibmw-install-prefix>"

# 1. Prepare a ROS2 interface package or package-shaped directory.
mkdir -p /tmp/demo_interfaces/msg
cat > /tmp/demo_interfaces/msg/Plain.msg <<'EOF'
float64 x
float64 y
int32[3] rgb
EOF

# 2. Import the ROS2 interface and request the JSON backend handoff artifacts.
python3 "$IBMW_GEN" import \
  --ros2-src /tmp/demo_interfaces \
  --artifact-only \
  --package-name demo_interfaces \
  --output-dir /tmp/demo_interfaces_gen \
  --target json

# Important generated files:
#   /tmp/demo_interfaces_gen/demo_interfaces_json_types.hpp
#   /tmp/demo_interfaces_gen/demo_interfaces_json_runtime_scaffold.ibmw.yaml
#   /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json

# 3. Generate a normal IBMW project from the scaffold YAML.
python3 "$IBMW_GEN" \
  /tmp/demo_interfaces_gen/demo_interfaces_json_runtime_scaffold.ibmw.yaml \
  -o /tmp/demo_json_runtime --force

# 4. Configure/build the generated project against an installed IBMW prefix.
cmake -S /tmp/demo_json_runtime -B /tmp/demo_json_runtime_build \
  -DCMAKE_PREFIX_PATH="$IBMW_INSTALL_PREFIX;/opt/ros/humble" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/demo_json_runtime_build -j$(nproc)
```

The generated JSON type header contains normal C++ structs with
`NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(...)` and a deterministic
`kTypeName`, for example `demo_interfaces/msg/Plain`. The scaffold YAML uses
`ros2_import_json_types_header` and `ros2_import_json_types_include_dir` so the
second `ibmw-gen` step can compile modules against the imported type header.

Reproducible verification is available through the unified wrapper:

```bash
# Reuse an existing IBMW install prefix for a fast check.
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_INSTALL_PREFIX" \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_verify

# Or let the lane build a minimal IBMW prefix in the work directory.
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_verify

# If a generated migration manifest selected json_scaffold, this also works:
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_INSTALL_PREFIX" \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json \
  /tmp/ibmw_json_verify

# Optional non-local smoke: run selected cases through HTTP publisher/subscriber processes.
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_NET_INSTALL_PREFIX" \
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_CASES=all \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_verify

# If IBMW_JSON_SMOKE_IBMW_PREFIX is unset, HTTP smoke builds a net-enabled
# prefix in the work directory before running the matrix.
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_CASES="plain bounded" \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_verify_build_prefix

# Optional installed-prefix reference HTTP smoke. The reference cases must also
# be generated by IBMW_JSON_SMOKE_REFERENCE_CASES.
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_NET_INSTALL_PREFIX" \
IBMW_JSON_SMOKE_REFERENCE_CASES=all \
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES=all \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_reference_verify
```

In manifest mode, the wrapper verifies that `backend_generated_files` lists the
JSON types header and runtime scaffold and that both files exist before it runs
the generic scaffold smoke.

For CI-friendly performance-boundary checks, run the manifest-only guard:

```bash
VERIFY_LANES=perf_boundary \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json \
  /tmp/ibmw_perf_boundary_verify
```

This emits `perf_boundary_summary.json` under the lane work directory. It checks
that the ROS2-compatible DDS lane remains rcl-free, that `dds_native` and
`iceoryx` are not promoted by metadata-only artifacts, and that zero-copy
eligibility is classified before future lowering work claims support. It does
not alter runtime hot-path code. Migration manifests select this lane
automatically when `dds_native` or `iceoryx` metadata sidecars are emitted. The
summary includes
`zero_copy_sized_interfaces`, which counts `iceoryx`-eligible interfaces whose
metadata includes a deterministic `max_serialized_size_bytes` bound.
When `iceoryx` is requested, import also emits
`<pkg>_iceoryx_lowering_plan.json`; it is a metadata-only plan for eligible and
blocked interfaces, future type/module names, and the compile/runtime promotion
gate, not generated runtime lowering.
When `dds_native` is requested, import emits
`<pkg>_dds_native_lowering_plan.json` with eligible/degraded interfaces, future
IDL/schema/type artifact names, and the schema/IDL compile-smoke then runtime
promotion gate. It is also plan-only.

The live `.msg`, `.srv`, and `.action` lanes emit summary artifacts after successful
matrix runs: `msg_live_matrix_summary.json`, `imported_srv_live_matrix_summary.json`,
`reference_srv_live_matrix_summary.json`, and
`action_runtime_matrix_summary.json`. These are CI-friendly evidence summaries;
the full lane logs remain the source of detailed debugging output.
`scripts/verify_ros2_interface_import_live.sh` now validates those summaries
after each live lane completes, so a lane cannot pass without the expected
machine-readable evidence artifact. Action summaries include stable
`action_type` and `action_name` fields for coverage dashboards.

Remaining optional coverage is queued as evidence-only work: expand real-prefix
reference `.msg` cases beyond the current 23-case local/HTTP matrix, add more
Nav2/MoveIt-style action runtime-template cases, broaden JSON non-local
transport coverage with explicit case selectors, and keep nested plain size
bounds healthy for native backend plans. None of these should promote a backend
or touch hot paths without the relevant gate.

Current verified JSON scaffold coverage:

- primitive scalar fields: `bool`, signed/unsigned integers, `float32`,
  `float64`
- fixed arrays such as `int32[3]`, `uint8[4]`, `float64[2]`
- strings and bounded/unbounded sequences such as `string<=16`, `uint8[<=8]`,
  and `uint8[]`
- same-package nested plain message fields
- source/workspace external-package nested message fields
- reference-existing external nested type/scaffold bundles, including a local
  runtime smoke for `std_msgs/msg/Header -> builtin_interfaces/msg/Time`
- real installed-prefix reference-existing local runtime smoke for
  `geometry_msgs/msg/PoseStamped`, `nav_msgs/msg/Odometry`, and
  `sensor_msgs/msg/Imu`, plus `geometry_msgs/msg/TransformStamped`,
  `geometry_msgs/msg/Twist`, `sensor_msgs/msg/LaserScan`, `nav_msgs/msg/Path`,
  `sensor_msgs/msg/PointCloud2`, `sensor_msgs/msg/NavSatFix`, and
  `geometry_msgs/msg/PoseWithCovarianceStamped`, plus
  `geometry_msgs/msg/PointStamped`, `geometry_msgs/msg/Vector3Stamped`,
  `geometry_msgs/msg/WrenchStamped`, `geometry_msgs/msg/AccelStamped`, and
  `sensor_msgs/msg/Image`, plus `sensor_msgs/msg/CameraInfo`,
  `sensor_msgs/msg/JointState`, `sensor_msgs/msg/BatteryState`,
  `sensor_msgs/msg/Range`, `sensor_msgs/msg/FluidPressure`,
  `sensor_msgs/msg/Temperature`, `nav_msgs/msg/OccupancyGrid`, and
  `nav_msgs/msg/MapMetaData` when `/opt/ros/humble` is available; use
  `IBMW_JSON_SMOKE_REFERENCE_CASES=all` to run the matrix. This coverage slice
  is verification-only and does not add runtime hot-path work.
- generated publisher/subscriber modules, package `.so` files, and app binaries
- same-process package-mode local transport smoke with publish and receive logs
- optional HTTP two-process package-mode smoke for all currently generated cases:
  `plain`, `scalars`, `fixed_array`, `bounded`, `dynamic`, `nested_detail`,
  `nested_top`, and `external_stamped`
- verification artifact `json_scaffold_generated_shape_matrix_summary.json`,
  emitted in the `json_scaffold` work directory with built-in generated shape
  local and optional HTTP evidence paths
- optional HTTP two-process smoke for real installed-prefix reference cases:
  `pose_stamped`, `odometry`, `imu`, `transform_stamped`, `twist`, and
  `laser_scan`, `path`, `point_cloud2`, `nav_sat_fix`, and
  `pose_with_covariance_stamped`, plus `point_stamped`, `vector3_stamped`,
  `wrench_stamped`, `accel_stamped`, `image`, `camera_info`, `joint_state`,
  `battery_state`, `range`, `fluid_pressure`, `temperature`, `occupancy_grid`,
  and `map_meta_data` via `IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES=all`
- verification artifact `json_scaffold_reference_matrix_summary.json`, emitted
  in the `json_scaffold` work directory with selected/generated/skipped case
  status plus local and optional HTTP evidence paths
- the unified wrapper validates both JSON summary artifacts after the lane runs,
  including selected totals, generated/local pass consistency, and optional HTTP
  requested/pass totals
- the `route_b` wrapper lane validates `route_b_target_artifact_audit.json` as
  machine-readable evidence for AArch64 target artifacts and host-prefix leaks
- optional CI timeout caps for delegated wrapper lanes via
  `IBMW_VERIFY_LANE_TIMEOUT_SECONDS` or per-lane overrides such as
  `IBMW_VERIFY_ROUTE_B_TIMEOUT_SECONDS`

Current boundaries:

- `json` is still reported as `degraded`, not fully `supported`.
- Import emits a handoff scaffold; it does not claim complete automatic JSON
  runtime lowering for every ROS2 message shape.
- HTTP inter-process smoke is opt-in and selected by
  `IBMW_JSON_SMOKE_HTTP_CASES` (`all` expands to the current full matrix);
  installed-prefix reference HTTP cases are selected separately by
  `IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES` and must also be present in
  `IBMW_JSON_SMOKE_REFERENCE_CASES`; broader non-local JSON coverage remains
  future work.
- When no `IBMW_JSON_SMOKE_IBMW_PREFIX` is supplied, HTTP smoke builds a local
  prefix with `IBMW_BUILD_NET_EXTENSION=ON`; this is slower but removes the
  need for a prebuilt net-enabled install.
- This JSON lane does not use `rcl`, `rclcpp`, or `rclpy` in the generated IBMW
  runtime path.
- Zero-copy / no-memcpy claims are not inferred from JSON or metadata coverage;
  `iceoryx` and native DDS promotion must pass the `perf_boundary` guard plus
  backend-specific compile/runtime evidence.
- `max_serialized_size_bytes` is a packed ROS2-IR metadata bound, not C++
  `sizeof(T)`; runtime chunk sizing must continue to use backend-specific
  compile/runtime evidence.
- Resolved nested plain `.msg` dependencies now contribute to that metadata
  bound in diagnosis and `dds_native` / `iceoryx` lowering plans.

## .ibmw.yaml Schema Reference

### project (required)

```yaml
project:
  name: my_project          # Project name (used for CMake project name)
  namespace: my::namespace   # C++ namespace for generated code
```

### modules (required)

Define the modules in your project. Each module becomes a C++ class with
`Setup(Runtime)`, `Run()`, `Stop()` lifecycle methods.

**Module types:**

| Type | Description | Transport |
|------|-------------|-----------|
| `bare` | No transport, no RPC. Pure lifecycle module. | None |
| `publisher` | Publishes messages on a topic. | DDS, ROS2, or JSON |
| `subscriber` | Subscribes to messages on a topic. | DDS, ROS2, or JSON |
| `rpc_server` | RPC service server. | DDS, ROS2, or JSON |
| `rpc_client` | RPC service client. | DDS, ROS2, or JSON |
| `logger` | Logging demo module with executor support. | None |
| `loaned_publisher` | Zero-copy publisher using `LoanedMessage` / iceoryx `LoanedSample`. | DDS (FastDDS DataSharing) or iceoryx |
| `loaned_subscriber` | Zero-copy subscriber consuming loaned SHM samples. | DDS or iceoryx |
| `direct_ros2_publisher` | Low-level ROS2 DDS publisher (no RCL). | DDS |
| `direct_ros2_subscriber` | Low-level ROS2 DDS subscriber (no RCL). | DDS |
| `ros2_interop_publisher` | *(Deprecated alias for `direct_ros2_publisher`.)* | DDS |
| `ros2_interop_subscriber` | *(Deprecated alias for `direct_ros2_subscriber`.)* | DDS |

**Example (DDS pub/sub):**
```yaml
modules:
  - name: PublisherModule
    type: publisher
    transport: dds
    topic: hello_topic
    message_type: ExampleEventMsg

  - name: SubscriberModule
    type: subscriber
    transport: dds
    topic: hello_topic
    message_type: ExampleEventMsg
```

**Example (ROS2 transport):**
```yaml
modules:
  - name: MyPublisher
    type: publisher
    transport: ros2
    topic: test_topic
    message_type: "example_ros2::msg::RosTestMsg"
    ros2_msg_include: "example_ros2/msg/ros_test_msg.hpp"
    ros2_package: example_ros2
```

**Example (ROS2 RPC with real code generation):**
```yaml
modules:
  - name: RpcServerModule
    type: rpc_server
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"

  - name: RpcClientModule
    type: rpc_client
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"
    custom_config:
      rpc_frq: 1.0
```

ROS2 RPC modules require `ros2_service`; ibmw-gen produces **real,
compilable** RPC code (typed service impl, client loop with SyncAccess, config
reading via yaml-cpp).

The generated config automatically includes `ros2_extension` loading and
`type: ros2` service driver configuration.

**Example (DDS RPC -- pure FastDDS, no RCL):**
```yaml
modules:
  - name: RpcServerModule
    type: rpc_server
    transport: dds
    dds_rpc_service: CalculatorService
    dds_rpc_namespace: dds_rpc_demo
    dds_rpc_methods: [Add]
    dds_rpc_gencode_target: "dds_rpc_demo::dds_rpc_demo_dds_idl_rpc_gencode"

  - name: RpcClientModule
    type: rpc_client
    transport: dds
    dds_rpc_service: CalculatorService
    dds_rpc_namespace: dds_rpc_demo
    dds_rpc_methods: [Add]
    dds_rpc_gencode_target: "dds_rpc_demo::dds_rpc_demo_dds_idl_rpc_gencode"
    custom_config:
      rpc_frq: 1.0
```

The generated config includes `dds_extension` loading and `type: dds` service
driver. With `ros2_compatible_mode: true` (default), the DDS topics follow
ROS2 naming conventions for transparent interop with native ROS2 nodes.

**Example (bare):**
```yaml
modules:
  - name: MyModule
    type: bare
    log_level: INFO           # Optional: default module log level
    executor_name: my_exec    # Optional: bind to an executor
```

#### `user_code` (optional, per module)

Fills a module's user-code region at generation time instead of the default
TODO placeholder. Each key names a region; its value is C++ code that
**replaces the whole default body** of that region (re-indented to the
region; inside the loaned publisher macro every line gets the aligned ` \`
continuation). Without `user_code` the output is unchanged. Because the user
`.h/.cc` files are only written once, use `--force` to re-emit them.

| Module type | Regions |
|---|---|
| `publisher`, `direct_ros2_publisher`, `direct_iceoryx_publisher` | `publish_body` |
| `subscriber`, `loaned_subscriber`, `direct_ros2_subscriber`, `direct_iceoryx_subscriber` | `on_message_body` |
| `loaned_publisher` | `populate_loaned_message` (macro body: `/* */` comments only) |
| `rpc_server` | `rpc_server_handler_body.<Method>` |
| `rpc_client` | `rpc_client_request_parameter_build.<Method>`, `rpc_client_response_parameter_consume.<Method>`, `rpc_client_return_status_handling.<Method>` |

```yaml
  - name: PublisherModule
    type: publisher
    transport: dds
    topic: hello_topic
    message: HelloMsg
    user_code:
      publish_body: |
        msg.id(msg_count_);
        msg.message("hello");
        msg_count_++;
```

Unknown regions, wrong module types, non-string values and lines that look
like IBMW marker anchors are rejected. `user_code` is emitted as code, so it
is exempt from the quote/backslash check that applies to other module values.

### apps (required)

Define runnable applications. Each app bundles one or more modules.

```yaml
apps:
  - name: my_app
    type: pubsub              # pubsub | rpc | bare
    modules: [PublisherModule, SubscriberModule]
```

**Per-app executors** (optional): Override project-level executors for a
specific app:

```yaml
apps:
  - name: executor_app
    type: bare
    modules: [MyModule]
    executors:
      - name: work_executor
        type: thread_pool
        options:
          thread_num: 2
```

**Config variants** (optional): Generate multiple YAML configs from one app
with different settings:

```yaml
apps:
  - name: logging_app
    type: bare
    modules: [LoggerModule]
    config_variants:
      - suffix: rotate_file
        logging:
          backends:
            - type: rotate_file
              options:
                path: ./log
                max_file_size_m: 4
```

### dds_types (optional)

Reference IDL-generated DDS types from the main IBMW build.

```yaml
dds_types:
  namespace: ibmw::protocols::example
  groups:
    - name: ExampleEvent
      idl_gencode_target: ibmw::schema::example_idl_gencode
      messages:
        - name: ExampleEventMsg
          fields:
            - { name: msg, type: string }
            - { name: num, type: int32 }
```

#### `dds_types.rpc_services` -- ROS2-compatible DDS RPC

Define DDS RPC services with native `.srv` support. The `srv:` field enables
automatic ROS2 DDS type name derivation and wire-compatible interop with
standard ROS2 services.

```yaml
dds_types:
  rpc_services:
    - name: AddTwoIntsService
      group: AddTwoInts
      srv: example_interfaces/srv/AddTwoInts  # Enables ROS2 interop
      # Optional overrides (auto-derived from srv if not set):
      # ros2_service_name: /custom_name
      # req_dds_type_name: example_interfaces::srv::dds_::AddTwoInts_Request_
      # rsp_dds_type_name: example_interfaces::srv::dds_::AddTwoInts_Response_
      methods:
        - name: AddTwoInts
          request: AddTwoInts_Request
          response: AddTwoInts_Response
```

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Service name used in generated code |
| `group` | Yes | IDL group name for type lookup |
| `srv` | No | ROS2 `.srv` package path (e.g. `example_interfaces/srv/AddTwoInts`). When set, auto-computes `req_dds_type_name`, `rsp_dds_type_name`, and `ros2_service_name` |
| `ros2_service_name` | No | Override the ROS2 service topic name (auto-derived from `srv` if not set) |
| `req_dds_type_name` | No | Override the DDS type name for the request |
| `rsp_dds_type_name` | No | Override the DDS type name for the response |
| `methods` | Yes | List of RPC methods (one per `.srv`) |

### custom_dds_types (optional)

Define custom DDS types directly (no IDL needed). ibmw-gen generates the C++
structs and FastCDR serialization code.

```yaml
custom_dds_types:
  namespace: "my::namespace"
  constants:
    - { name: kBufferSize, type: uint64, value: 1048576 }
  enums:
    - name: Encoding
      underlying_type: uint32
      values:
        - { name: RGB8, value: 0 }
        - { name: GRAY8, value: 2 }
  messages:
    - name: Header
      constructor_args: true
      fields:
        - { name: frame_id, type: uint64, default: "0" }
        - { name: timestamp, type: float64, default: "0.0" }
    - name: ImageMsg
      constructor_args: true
      fields:
        - { name: header, type: Header }          # Nested message
        - { name: encoding, type: Encoding }       # Enum type
        - { name: data, type: "array<uint8, kBufferSize>" }
```

**Supported field types:** `bool`, `int8`, `int16`, `int32`, `int64`, `uint8`,
`uint16`, `uint32`, `uint64`, `float32`, `float64`, `string`,
`array<T, N>`, `vector<T>`, user-defined enum types, and other
previously-defined message types (nested structs).

**Nested message types:** Fields can reference other messages defined in the
same `custom_dds_types` section. Supported forms:

- Direct: `{ name: header, type: Header }`
- Vector: `{ name: objects, type: "vector<SceneObject>" }`
- Fixed array: `{ name: points, type: "array<Vec3, 4>" }`

Messages may reference each other freely (forward references are resolved
automatically), but circular references (A depends on B depends on A) are
rejected.

### experimental_actions (optional, experimental)

`experimental_actions` is a narrow experimental lane for the DDS-only
ROS2-compatible Fibonacci Action proof. It exists to compile and link the
current Action interop slice through `ibmw-gen`; it is not a public Action
runtime, not a generic `.action` parser, and it does not add `action_server` or
`action_client` module types.

```yaml
experimental_actions:
  - name: fibonacci
    action: example_interfaces/action/Fibonacci
    transport: dds
    roles: [server, client]
```

Restrictions are intentional:

- `action` must be `example_interfaces/action/Fibonacci`
- `transport` must be `dds`
- `roles` may contain `server`, `client`, or both
- generated CMake must not link `rcl`, `rclcpp`, `rclcpp_action`, `rcl_action`,
  or `rmw`
- generated sources stay under `src/experimental_actions/fibonacci/`

See `src/samples/cpp/ros2_action_fibonacci_gen/` for the in-tree generator
sample. Runtime interop validation remains in the hand-written sibling samples
`ros2_action_fibonacci_dds_server/` and `ros2_action_fibonacci_dds_client/`.

### executors (optional)

Define project-level executors shared by all apps (unless overridden per-app).

```yaml
executors:
  - name: work_executor
    type: thread_pool
    options:
      thread_num: 2
  - name: strand_executor
    type: strand
    options:
      bind_thread_pool_executor_name: work_executor
```

**Executor types:** `thread_pool`, `strand`, `dedicated`, `timer`,
`parallel`, `time_manipulator`

**Real-time options (thread_pool):**
```yaml
executors:
  - name: rt_thread
    type: thread_pool
    options:
      thread_num: 1
      thread_sched_policy: "SCHED_FIFO:80"
      thread_bind_cpu: [0, 1]
      timeout_alarm_threshold_us: 100
```

### dds_driver (optional)

Configure the DDS extension driver with ROS2 interoperability.

```yaml
dds_driver:
  domain_id: 0
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"
```

### logging (optional)

Configure log backends and levels.

```yaml
logging:
  core_level: INFO
  backends:
    - type: console
    - type: rotate_file
      options:
        path: ./log
        filename: app.log
        max_file_size_m: 4
        max_file_num: 10
```

### json_types (optional)

Define JSON-serializable types. The generator emits:

- `src/json_custom_types.h`: C++ types, JSON serialization, and JSON Schema.
- `src/json_type_support_pkg_main.cc`: TypeSupport/SchemaSupport package entrypoint.
- `<project>_json_ts`: shared library target loadable by the Bag TypeSupportPkgLoader.

```yaml
json_types:
  messages:
    - name: ConfigData
      fields:
        - { name: name, type: string }
        - { name: value, type: int32 }
```

## Generated File Structure

For a typical `my_project` with one publisher + one subscriber using
`custom_dds_types`:

```
my_project_gen/
  my_project.ibmw.yaml          # Copy of input (for re-generation)
  CMakeLists.txt                 # Build system
  cfg/
    my_app.yaml                  # App-mode config
    my_app_pkg.yaml              # Pkg-mode config (one pkg per app)
  protocol/
    my_message.h                 # Custom type header (struct + CDR)
  src/
    json_custom_types.h            # JSON types with embedded JSON Schema
    json_type_support_pkg_main.cc  # JSON TypeSupport/SchemaSupport package entrypoint
    my_app/
      main.cc                    # App entry point (auto-generated)
    publisher_module/
      publisher_module.h         # Module header (YOU EDIT THIS)
      publisher_module.cc        # Module impl (YOU EDIT THIS)
    subscriber_module/
      subscriber_module.h        # Module header (YOU EDIT THIS)
      subscriber_module.cc       # Module impl (YOU EDIT THIS)
    my_app_pkg/
      pkg_main.cc                # Pkg loader (auto-generated, one per app)
```

**Pkg naming rules:** If an app has one module, the pkg is named
`<module>_pkg`. If it has multiple modules, the pkg is named `<app>_pkg`
(or `<app_without_app_suffix>_pkg` if the app name ends in `_app`).

## Workflow

1. **Define** your project in a `.ibmw.yaml` file
2. **Generate** with `ibmw-gen`
3. **Edit** the module `.h`/`.cc` files -- fill in the TODO markers
4. **Build** with CMake
5. **Run** in App mode or Pkg mode
6. **Iterate** -- re-run `ibmw-gen` when you change the YAML; your module
   edits are preserved

## Examples

Each demo under `src/samples/cpp/` has a `.ibmw.yaml` file. Run `ibmw-gen` on
it to generate the corresponding project:

| Demo | ibmw.yaml Features | Module Types |
|------|-------------------|--------------|
| `quickstart` | Minimal bare module | `bare` |
| `transport_demo` | DDS pub/sub with IDL types | `publisher`, `subscriber` |
| `ros2_chn` | ROS2 message transport | `publisher`, `subscriber` |
| `ros2_rpc` | ROS2 RPC server/client with real code | `rpc_server`, `rpc_client` |
| `dds_native_demo` | Custom DDS types | `publisher`, `subscriber` |
| `dds_image_demo` | Custom types + enums + arrays | `publisher`, `subscriber` |
| `logging_demo` | Logging backends + config_variants | `logger` |
| `config_param_demo` | Executors + parameter API | `bare` |
| `ros2_interop_demo` | ROS2 DDS interop (no RCL) | `direct_ros2_publisher/subscriber` |
| `ros2_point_demo` | ROS2 geometry_msgs + zero-copy | `direct_ros2_publisher/subscriber` |
| `executor_demo` | Per-app executors + real-time | `bare` (5 modules) |
| `dds_rpc_demo` | Pure-DDS RPC with native `.srv` + ROS2 interop | `rpc_server`, `rpc_client` |
| `ros2_action_fibonacci_gen` | Experimental DDS-only Fibonacci Action sample generated by `experimental_actions` | -- |
| `multi_driver_demo` | Single module bound to **two** drivers (DDS + Iceoryx) via `IBMW_USE_DRIVERS(...)` | `bare` (with multi-driver typed slots) |
| `r12_validation` | Reverse-proof samples for the red-line scanner. Not for production use. | -- |

## Multi-Driver Modules

A single module can be bound to multiple transport drivers (e.g., DDS and
Iceoryx) using the variadic macro declared in `src/api/cpp/module_base.h`:

```cpp
IBMW_USE_DRIVERS(
    IBMW_DRIVER_ENTRY(DdsDriverT, dds_driver_),
    IBMW_DRIVER_ENTRY(IceoryxDriverT, iceoryx_driver_))
```

The host runtime feeds each driver into a typed slot at startup via the package
ABI; user code accesses each driver through its declared member pointer with no
stringified lookup.

See `src/samples/cpp/multi_driver_demo/` for a complete worked example
(DDS + Iceoryx, available in managed, standalone, generated, and pkg modes).

## Verification you should run after editing a yaml

```bash
# Pytest: locks generator output against golden files.
cd src/tools/ibmw_gen && python3 -m pytest -q

# In-tree regen: every in-tree yaml must regenerate byte-equal.
bash scripts/regen_in_tree.sh
```

## Current limitations

- **Single IDL external target per group**: Each DDS type group references one
  `idl_gencode_target`
- **Action generation is experimental and Fibonacci-only**: `experimental_actions`
  supports only `example_interfaces/action/Fibonacci` over DDS and is kept out
  of the public module API.

> **Note on zero-copy:** `loaned_publisher` / `loaned_subscriber` are fully
> supported on both DDS (FastDDS DataSharing for plain types) and iceoryx
> (native SHM for trivially-copyable types). See
> `src/samples/cpp/loaned_message_demo/` for end-to-end examples.

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
