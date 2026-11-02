# IBMW -- Intelligence Boom Middleware Framework

IBMW (Intelligence Boom Middleware) is a modular, extension-based middleware
framework for robotics and real-time systems. It provides a unified runtime with
pluggable communication backends (DDS, gRPC, MQTT, TCP/UDP, ROS2), executor-based
task scheduling, structured logging, parameter management, and an RPC framework --
all with DDS IDL / FastCDR as the primary type system.

**Version:** 0.1.0

## Features

- **Extension architecture** -- Load communication backends, loggers, and services
  as shared-library extensions at runtime
- **DDS-native type system** -- IDL-defined types with FastCDR serialization;
  no protobuf dependency
- **Multiple communication backends** -- DDS (FastDDS), gRPC, MQTT, TCP/UDP,
  local (in-process), ROS2
- **Executor framework** -- Async task scheduling with thread pools, strands,
  timers, coroutines (C++20), and real-time executors with CPU affinity
- **RPC framework** -- IDL-based request/response with pluggable backends
  and four calling patterns: synchronous, async callback, future, coroutine
- **Transport (pub/sub)** -- Topic-based publish/subscribe with backend routing
- **Zero-copy support** -- Loaned message API for shared-memory transports:
  Iceoryx, or the optional self-developed `shm` transport
  (`IBMW_BUILD_SHM_EXTENSION`, no RouDi daemon, Linux/OHOS; see
  [usage and migration](selfdev/shm/docs/使用与迁移.md), in Chinese)
- **Multi-driver modules** -- Bind multiple transport drivers (e.g., DDS +
  Iceoryx) to typed slots in a single module via the variadic
  `IBMW_USE_DRIVERS(...)` macro
- **Context API** -- Module lifecycle management with resource isolation
- **ROS2 interoperability** -- Direct FastDDS communication with ROS2 nodes;
  product endpoints do not require RCL/RCLCPP or an RMW implementation, while
  a static message closure may retain ROSIDL typesupport, `rcutils`, and base
  `librmw` ABI support. Pub/sub has scoped evidence; RPC/Service requires its
  protocol-specific acceptance gate.
- **DDS RPC** -- Pure-DDS RPC driver for the ROS2 Service protocol via
  WriteParams/SampleInfo, without rcl/rclcpp; compatibility claims remain
  scoped to the validated release, DDS profile, types, and acceptance evidence.
- **C++20** -- Coroutine support, concepts, structured bindings

## Documentation

| Document | Description |
|----------|-------------|
| [Documentation](docs/README.md) | Public documentation index for users, contributors, ecosystem resources, and releases |
| [Compatibility](docs/reference/compatibility.md) | Stable ABI, zero-copy, ROS2, Action, cross-build, and dependency constraints |
| [Troubleshooting](docs/guides/troubleshooting.md) | FastDDS/FastCDR, GLIBCXX, discovery, p99, include-lane, and process cleanup guidance |
| [User Guide](docs/guides/user-guide.md) | Getting started, configuration, running demos, YAML reference |
| [Developer Guide](docs/contributing/developer-guide.md) | Build system, code structure, writing modules and extensions |
| [Cross-compilation Configure Guide](docs/guides/cross-compilation.md) | aarch64 Linux configure approach, target dependency model, and scoped validation boundaries |
| [OHOS Xiaoyi Humble Cross-build Guide, Chinese](docs/guides/ohos-xiaoyi-humble-cross-build-guide.zh.md) | OHOS ARM64 compile-only guide for the Xiaoyi Humble environment |
| Minimal ROS2 build/deploy | Requires the separate `IBMW_together` companion repository. Start with `IBMW_together/ros2_minimal_deps/docs/ohos-std-msgs-string-cross-build-deploy-interop.zh.md`; the [IBMW guide](docs/guides/ohos-dds-ros2-minimal-interop.zh.md) is a concept/boundary index only |
| [ROS2 Interface Import User Guide](docs/guides/ros2-interface-import-user-guide.md) | User-facing guide for `ibmw-gen import` modes, artifacts, and verification lanes |
| [ROS2 Action DDS-only Adapter Guide](docs/experimental/ros2-action-dds-only-adapter-guide.md) | Experimental low-code developer guide for DDS-only Action servers |
| [API Reference](docs/reference/api-reference.md) | Complete C++ SDK, C ABI, extension, and package API |
| [Iceoryx Benchmark](docs/guides/transport-benchmark.md) | 7-sample zero-copy vs DDS vs TCP latency tutorial with reproduction recipe |
| [Performance Collection Guide, Chinese](docs/guides/performance-collection.zh.md) | ROS2/IBMW collection, validation, Core Metrics, artifacts, and A/B/C prototype boundaries |
| [Performance Observability Design, Chinese](docs/architecture/performance-observability-design.zh.md) | Unified measurement contracts and implemented/planned capability boundaries |
| [Performance Observability Implementation, Chinese](docs/contributing/performance-observability-implementation.zh.md) | Stable delivery and maintenance boundaries for the three observability workstreams |
| ROS2 latency clock contract | A producer `completion` record includes `clock_domain`; the only supported value is `producer_monotonic`. Legacy or unknown values are diagnostic-only and produce no metric or comparison conclusion |
| [Ecosystem Resources](docs/ecosystem/README.md) | Human-facing indexes for public migration and SDK assets |
| [Release Notes](docs/releases/0.1.0.md) | Public notes and compatibility limits for version 0.1.0 |
| [ibmw-template](ibmw-template/) | Starter project with 8 demos plus an extension-loading example covering the supported message type systems |
| [ibmw-gen](src/tools/ibmw_gen/) | Code generator -- produce a full project from a single `.ibmw.yaml` |

## Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| OS | Linux (Ubuntu 22.04+) | |
| Compiler | GCC 11+ or Clang 14+ | C++20 required |
| CMake | 3.24+ | |
| ROS 2 Humble | (optional, native) | Native full builds may source ROS 2 only for FastDDS/FastCDR headers, libraries and generator metadata: `sudo apt install ros-humble-fastrtps`. The rcl runtime is NOT required. For AArch64 cross builds, use the source-built DDS prefix workflow in [the cross-compilation guide](docs/guides/cross-compilation.md), not the host ROS 2 install. |
| FastCDR | 2.x | Built from source or from `_deps/` |

## Build

IBMW uses [CMake Presets](CMakePresets.json) for standard build configurations:

```bash
# Native build: source ROS 2 environment for FastDDS/FastCDR headers/libraries
# and host generator metadata only — no rcl runtime needed. AArch64 cross builds
# use the source-built DDS prefix workflow in docs/guides/cross-compilation.md instead.
# The `set +u` / `set -u` envelope is required because /opt/ros/humble/setup.bash
# references unset shell variables internally.
set +u
source /opt/ros/humble/setup.bash
set -u

# Recommended rcl-free build: DDS transport plus build-time ROS2 message types.
# Do not enable deprecated IBMW_BUILD_WITH_ROS2/IBMW_BUILD_ROS2_EXTENSION here.
# The full preset includes the self-developed shm transport, which needs a
# Linux kernel; on other hosts add -DIBMW_BUILD_SHM_EXTENSION=OFF.
cmake --preset full -DIBMW_BUILD_TESTS=ON -DIBMW_BUILD_BENCHMARKS=ON \
                    -DIBMW_BUILD_DDS_EXTENSION=ON -DIBMW_BUILD_ROS2_TYPES=ON \
                    -DIBMW_BUILD_WITH_ROS2=OFF -DIBMW_BUILD_ROS2_EXTENSION=OFF
cmake --build build -j$(nproc)

# Generate IDL code targets (required before install).
# The actuator schema now follows the same `_idl_gencode` naming lane as the
# other schema targets. `dds_ext_proto` is the DDS extension protobuf adapter
# target.
cd build && make ibmw_schema_common_idl_gencode \
  ibmw_schema_geometry_idl_gencode \
  ibmw_schema_sensor_idl_gencode \
  ibmw_schema_actuator_idl_gencode \
  dds_ext_proto

# Install (copies binaries, libs, configs into build/)
cmake --install . --prefix .

# Remove stale config files that interfere with ibmw-gen projects
rm -f ibmw-config.cmake ibmw-config-version.cmake
```

### Available Presets

| Preset | Description |
|--------|-------------|
| `minimal` | Core runtime only, fastest build for development |
| `dev` | Core + common extensions + examples (Debug) |
| `full` | **Recommended.** All extensions + examples + ROS2 types (Release) |
| `together-ros2-minimal-sdk` | Humble DDS compatibility build: DDS extension/types ON; ROS2 runtime and examples OFF |
| `ros2` | Full + ROS2 rcl integration (**deprecated**, see below) |
| `release` | Optimized build without examples |
| `test` | Dev build with tests enabled |

For an already authenticated Humble DDS prefix, the exact preset commands are:

```bash
export IBMW_ROS2_MINIMAL_SDK_PREFIX=/absolute/path/to/dds-prefix
cmake --preset together-ros2-minimal-sdk
cmake --build --preset together-ros2-minimal-sdk
```

The sibling `ros2_minimal_deps/scripts/ibmw_dds_compat.py` CLI is the recommended
entrypoint for OHOS ARM64 because it diagnoses the prefix/toolchain, owns a
dedicated build directory, and validates the resulting CMake cache.
OpenCode users can invoke `/dds-compatibility`. `IBMW_together` is a separate companion repository, not part of this repository: check it out next to this repository first, or the commands below fail with file not found.
Copyable sibling workflow:
```bash
python3 ../IBMW_together/ros2_minimal_deps/scripts/ibmw_dds_compat.py doctor --target ohos-arm64 --dds-prefix /absolute/prefix --allow-network
python3 ../IBMW_together/ros2_minimal_deps/scripts/ibmw_dds_compat.py run --target ohos-arm64 --dds-prefix /absolute/prefix --allow-network
```
Default mode requires a message-complete prefix with the `std_msgs` config/header. DDS substrate-only builder output requires `--dds-only`. Prefix precedence is explicit `--dds-prefix`, then `IBMW_ROS2_MINIMAL_SDK_PREFIX`, then exactly one canonical candidate. Offline use requires a complete prepopulated CMake FETCHCONTENT_BASE_DIR tree containing every required source layout. If `/dds-compatibility` is absent after adding/updating the skill, restart OpenCode.

> **ROS2 rcl Deprecation Notice**
>
> The `ros2` preset and `IBMW_BUILD_WITH_ROS2=ON` enable components that
> depend on the ROS2 rcl/rclcpp runtime (`ros2_extension`, ROS2 RPC driver).
> These components are **deprecated** and will be removed in a future release.
>
> The `full` preset includes `IBMW_BUILD_ROS2_TYPES=ON`, which builds ROS2
> type support using rosidl at build time only (no rcl runtime dependency).
> Demos like `ros2_interop_demo`, `ros2_point_demo`, and `loaned_message_demo`
> work with the `full` preset.
>
> IBMW now provides pure-DDS alternatives for all ROS2 interop scenarios:
>
> | Scenario | Deprecated (rcl) | Replacement (pure DDS) |
> |----------|------------------|----------------------|
> | Pub/Sub with ROS2 | `ros2_extension` + `type: ros2` transport | `dds_extension` + `ros2_compatible: true` |
> | RPC/Service with ROS2 | `ros2_extension` + `type: ros2` service | `dds_extension` + `type: dds` service with `ros2_compatible: true` |
> | ROS2 message types | `ros2_types` (links rclcpp) | `dds_types` with custom IDL |
>
> The `full` preset builds everything you need without any ROS2 runtime
> dependency. Native builds may source `ros-humble-fastrtps` for FastDDS/FastCDR
> metadata; AArch64 cross builds should use the source-built DDS prefix workflow
> instead of host ROS 2.

### Manual build (without presets)

```bash
# Native build: source ROS 2 for FastDDS/FastCDR headers/libraries and host
# generator metadata only — no rcl runtime needed. AArch64 cross builds use the
# source-built DDS prefix workflow instead.
source /opt/ros/humble/setup.bash

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ECHO_EXTENSION=ON \
  -DIBMW_BUILD_NET_EXTENSION=ON \
  -DIBMW_BUILD_GRPC_EXTENSION=ON \
  -DIBMW_BUILD_MQTT_EXTENSION=ON \
  -DIBMW_BUILD_PROXY_EXTENSION=ON \
  -DIBMW_BUILD_PARAMETER_EXTENSION=ON \
  -DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON \
  -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON \
  -DIBMW_BUILD_TIME_MANIPULATOR_EXTENSION=ON \
  -DIBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION=ON \
  -DIBMW_BUILD_TOPIC_LOGGER_EXTENSION=ON

cmake --build build -j$(nproc)
```

## Quick Start

After building, executables and configs are in `build/`. Set up the environment:

```bash
source /opt/ros/humble/setup.bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:$LD_LIBRARY_PATH"
```

### Hello World

```bash
# Pkg mode -- module loaded as .so by ibmw runtime
./bin/ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml

# App mode -- standalone executable
./helloworld_app ./cfg/examples_cpp_helloworld_app_mode_cfg.yaml
```

### Pub/Sub (local backend, single process)

```bash
./bin/ibmw run ./cfg/pubsub_single_process_cfg.yaml
```

### Pub/Sub (DDS backend, two processes)

```bash
# Terminal 1
./bin/ibmw run ./cfg/publisher_dds_cfg.yaml

# Terminal 2
./bin/ibmw run ./cfg/subscriber_dds_cfg.yaml
```

### DDS Native (raw DDS pub/sub)

```bash
./dds_native_app ./dds_native_cfg.yaml
```

### DDS Image (high-throughput 640x480 RGB at 10Hz)

```bash
./dds_image_app ./dds_image_cfg.yaml
```

### DDS RPC (pure-DDS service, ROS2-compatible)

This is an ibmw-gen demo. Generate and build it first:

```bash
cd src/samples/cpp/dds_rpc_demo
python3 ../../../tools/ibmw_gen/ibmw_gen.py dds_rpc_demo.ibmw.yaml -o dds_rpc_demo_gen
cd dds_rpc_demo_gen && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
export IBMW_BUILD_DIR="<ibmw-build-dir>"
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# Set up runtime environment
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$IBMW_BUILD_DIR/lib:$IBMW_BUILD_DIR:$IBMW_BUILD_DIR/bin:$LD_LIBRARY_PATH"

# Terminal 1: Server
./dds_rpc_server_app cfg/dds_rpc_server_app.yaml

# Terminal 2: Client
./dds_rpc_client_app cfg/dds_rpc_client_app.yaml
```

See the [User Guide](docs/guides/user-guide.md) for the full list of demo applications.

## Starting a New Project

The fastest way to create a new IBMW project is with **ibmw-gen**:

```bash
# 1. Write a .ibmw.yaml defining your modules, messages, and services
# 2. Generate
python3 src/tools/ibmw_gen/ibmw_gen.py my_project.ibmw.yaml -o my_project

# 3. Build
cd my_project && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
export IBMW_BUILD_DIR="<ibmw-build-dir>"
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 4. Run
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$IBMW_BUILD_DIR/lib:$IBMW_BUILD_DIR:$IBMW_BUILD_DIR/bin:$LD_LIBRARY_PATH"
./my_app cfg/my_app.yaml
```

Alternatively, copy [ibmw-template/](ibmw-template/) and modify the
hand-written demos directly. See the
[ibmw-template README](ibmw-template/README.md) for a full walkthrough.

## Architecture

```
ibmw/
  src/
    core/                       # Core runtime engine
    |  engine/                  #   Subsystem managers (executor, transport,
    |  |                        #     rpc, logger, module, extension, memory,
    |  |                        #     parameter, config, watchdog, main_loop)
    |  main/                    #   Entry point (ibmw binary)
    |  extension_api/           #   Extension development API
    api/                        # Public interfaces for module developers
    |  c/                       #   C ABI layer (vtable-based, ABI-stable)
    |  cpp/                     #   C++ SDK (Runtime, Context, Transport, RPC,
    |  |                        #     Executor, Logger, Coroutine utilities)
    |  pkg/                     #   Package ABI (dynlib entry points)
    |  type_support/            #   Type support ABI (serialization plugins)
    extensions/                 # Pluggable extensions
    |  transport/               #   DDS, Net (TCP/UDP), MQTT, ROS2, Iceoryx, Shm, Zenoh
    |  rpc/                     #   gRPC
    |  observability/           #   Log Control, Topic Logger, OpenTelemetry
    |  management/              #   Parameter, Time Manipulator, Service
    |  |                        #     Introspection, Record/Playback
    |  utility/                 #   Echo (testing), Proxy
    schema/                     # Type definitions
    |  dds_idl/                 #   DDS IDL files + generated FastCDR code
    |  ros2/                    #   ROS2 message/service definitions
    |  extensions/              #   Extension-specific type definitions
    samples/cpp/                # Example applications + verification samples
    tools/                      # CLI tool, IDL code generators, ibmw-gen
  cmake/                        # 30 CMake modules (deps, codegen)
  docs/                         # Documentation
  ibmw-template/                # External starter project with 8 demos plus an extension-loading example
```

### Module Lifecycle

1. **Setup(Runtime)** -- Module receives `Runtime` handle, registers transport types and RPC services
2. **Run()** -- Module begins active work (publish loops, RPC handling, timers)
3. **Stop()** -- Module stops work, releases resources, signals completion

### Execution Modes

| Mode | Description | Entry Point |
|------|-------------|-------------|
| **Pkg** | Modules as `.so` loaded by `ibmw` runtime | `./bin/ibmw run config.yaml` |
| **App** | Standalone executable with modules compiled in | `./my_app config.yaml` |
| **Registration** | Modules registered programmatically in `main()` | Custom `main()` |

### Executor Types

| Type | Description |
|------|-------------|
| `thread_pool` | Asio-based async thread pool with configurable thread count |
| `strand` | Serialized (ordered) execution bound to a `thread_pool` |
| `parallel` | TBB-based parallel executor for compute-intensive workloads |
| `dedicated` | Single dedicated worker thread with its own queue |
| `timer` | Timer-wheel based scheduled/periodic execution |

## Examples

All examples are in `src/samples/cpp/` and built to `build/`.

| Example | Description | Backend | Mode | Requires |
|---------|-------------|---------|------|----------|
| `quickstart` | Minimal module lifecycle (Hello World) | -- | pkg, app, registration | -- |
| `executor_demo` | Thread pools, strands, timers, coroutines, real-time scheduling | -- | pkg | -- |
| `logging_demo` | Log levels, formatting, file rotation, benchmarks | console, file | pkg | -- |
| `config_param_demo` | Runtime parameter get/set | -- | pkg | -- |
| `context_demo` | Context API: executor, logger, transport, RPC | local | pkg | -- |
| `transport_demo` | Pub/sub messaging with local and DDS backends | local, DDS | pkg, app | -- |
| `dds_native_demo` | Raw DDS pub/sub with hand-written types | DDS | app | -- |
| `dds_image_demo` | High-throughput image streaming (~1MB/frame at 10Hz) | DDS | app | -- |
| `dds_rpc_demo` | Pure-DDS RPC server/client, ROS2-compatible | DDS | app, pkg | -- |
| `loaned_message_demo` | Zero-copy LoanedMessage API, 3 config modes | DDS | app | ROS2 types (rosidl build-time) |
| `ros2_chn` | ROS2 msg-based transport pub/sub | ROS2 (local) | pkg, app | **rcl (deprecated)** |
| `ros2_rpc` | RPC with sync/async/future/coroutine patterns | ROS2 (local) | pkg, app | **rcl (deprecated)** |
| `ros2_interop_demo` | ROS2 interop without RCL/RMW dependency | DDS | app | ROS2 types (rosidl build-time) |
| `ros2_point_demo` | Zero-copy with ROS2 Plain types (geometry_msgs::Point) | DDS | app | ROS2 types (rosidl build-time) |
| `iceoryx_inproc_demo` | Single-process Iceoryx pub/sub (RouDi managed by helper script) | Iceoryx | app | RouDi (auto-started) |
| `iceoryx_interproc_demo` | Two-process Iceoryx pub/sub with shared-memory zero-copy | Iceoryx | app | RouDi (auto-started) |
| `multi_driver_demo` | Single module bound to **two** drivers (DDS + Iceoryx) via `IBMW_USE_DRIVERS` | DDS + Iceoryx | app, pkg | DDS + Iceoryx extensions |

## Benchmarking & Verification

End-to-end latency verification across 7 transport samples (Iceoryx,
FastDDS, net_extension shared-memory, etc.) with a standard 6-payload
size matrix (1KiB / 4KiB / 16KiB / 64KiB / 256KiB / 1MiB).

```bash
source /opt/ros/humble/setup.bash
scripts/run_bench_sweep.sh --reset-csv
```

The self-developed shm transport adds the `shm_send`, `shm_loan_v2`,
`shm_loaned_pod` and `shm_loaned_ctx` samples (select them with
`IBMW_BENCH_SAMPLES`; they also build without Iceoryx).

The sweep script handles per-sample SHM cleanup, full 6×7 CSV emission,
and is documented step-by-step in
[the English benchmark guide](docs/guides/transport-benchmark.md) and
[the Chinese benchmark guide](docs/guides/transport-benchmark.zh.md).

> **Note on the legacy STATIC bench harness.** The previous static-payload
> benchmark path (gated by `IBMW_BENCH_STATIC_PAYLOAD`) and its
> hand-written `cdr_impl.cc` shim have been removed. All benchmarks now go
> through the unified `transport_benchmark` payload registry (X-macro driven)
> and the `ibmw-template` scaffolding (scheme D-full).

## YAML Configuration

All configuration lives under the `ibmw:` top-level key. Module-specific config
uses separate top-level keys matching the module name.

```yaml
ibmw:
  extensions:
    items:
      - name: dds_extension
        path: libibmw_dds_transport.so
        options:
          domain_id: 0

  logging:
    core_level: Info
    backends:
      - type: console
        options: { color: true }

  scheduling:
    - name: work_pool
      type: thread_pool
      options: { thread_num: 4 }

  messaging:
    drivers:
      - type: dds
    publishers:
      - topic: ".*"
        enable_drivers: [dds]
    subscribers:
      - topic: ".*"
        enable_drivers: [dds]

  modules:
    packages:
      - path: ./libmy_module_pkg.so
    modules:
      - name: MyModule
        log_level: Info

# Per-module custom config
MyModule:
  publish_rate_hz: 10
  output_topic: /data
```

See the [User Guide](docs/guides/user-guide.md) for the complete YAML configuration reference.

## Extensions

| Extension | Library | Description |
|-----------|---------|-------------|
| DDS | `libibmw_dds_transport.so` | FastDDS transport (pub/sub) + pure-DDS RPC backend (ROS2-compatible) |
| gRPC | `libibmw_grpc_transport.so` | gRPC RPC backend |
| Net | `libibmw_net_transport.so` | HTTP/TCP/UDP transport + RPC backend |
| MQTT | `libibmw_mqtt_transport.so` | MQTT transport backend |
| ROS2 | `libibmw_ros2_transport.so` | ROS2 transport backend (**deprecated** -- use DDS extension instead) |
| Echo | `libibmw_echo_driver.so` | Echo RPC backend (testing) |
| Proxy | `libibmw_proxy_driver.so` | Topic proxy/forwarding backend |
| Log Control | `libibmw_log_control.so` | Runtime log level control via RPC |
| Parameter | `libibmw_parameter_service.so` | Parameter get/set service |
| Record/Playback | `libibmw_record_playback.so` | Message recording to bag files |
| Time Manipulator | `libibmw_time_manipulator.so` | Time manipulation for testing |
| Topic Logger | `libibmw_topic_logger.so` | Publish log messages to topics |
| Service Introspection | `libibmw_service_introspection.so` | Service discovery and introspection |

## License

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. All rights reserved.
