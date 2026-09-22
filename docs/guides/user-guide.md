# IBMW User Guide

This guide covers getting started with IBMW, configuration, running demos, and
the complete YAML configuration reference.

## Table of Contents

1. [Getting Started](#getting-started)
2. [Build Options](#build-options)
3. [Running Demos](#running-demos)
4. [Demo Reference](#demo-reference)
5. [YAML Configuration Reference](#yaml-configuration-reference)
6. [Troubleshooting](#troubleshooting)

---

## Getting Started

### Installation

```bash
# Install prerequisites (Ubuntu 22.04+)
sudo apt update
sudo apt install build-essential cmake gcc-11 g++-11
# Native FastDDS/FastCDR metadata (rcl runtime is NOT required).
# For AArch64 cross builds, use the source-built DDS prefix workflow in
# docs/guides/cross-compilation.md instead of host ROS 2.
sudo apt install ros-humble-fastrtps

# Native build: source ROS 2 for FastDDS/FastCDR headers/libraries and host
# generator metadata only. The set +u / set -u envelope is required because
# /opt/ros/humble/setup.bash references unset shell variables.
set +u
source /opt/ros/humble/setup.bash
set -u

# Recommended rcl-free build: DDS transport plus build-time ROS2 message types.
# Do not enable deprecated IBMW_BUILD_WITH_ROS2/IBMW_BUILD_ROS2_EXTENSION here.
cmake --preset full -DIBMW_BUILD_TESTS=ON -DIBMW_BUILD_BENCHMARKS=ON \
                    -DIBMW_BUILD_DDS_EXTENSION=ON -DIBMW_BUILD_ROS2_TYPES=ON \
                    -DIBMW_BUILD_WITH_ROS2=OFF -DIBMW_BUILD_ROS2_EXTENSION=OFF
cmake --build build -j$(nproc)

# Generate IDL code targets (required before install)
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

> **Note:** The `full` preset builds all extensions and examples without any
> ROS2 rcl runtime dependency. If you need the deprecated ROS2 rcl integration,
> use `cmake --preset ros2` (requires full ROS2 installation).
> AArch64 DDS cross builds use the source-built DDS prefix workflow documented
> in `docs/guides/cross-compilation.md`; do not use host `/opt/ros/humble` as the
> target dependency source.

### Running Your First Module

```bash
source /opt/ros/humble/setup.bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:$LD_LIBRARY_PATH"

# Run the Hello World example
./bin/ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml
```

You should see:

```
IBMW v0.1.0 starting...
Info ... [core] IBMW v0.1.0 | IB Middleware Framework
Info ... [HelloWorldModule] Setup
Info ... [HelloWorldModule] Run
```

Press `Ctrl+C` to shut down.

### Three Execution Modes

IBMW supports three ways to run modules:

**Pkg mode** -- The `ibmw` binary loads module shared libraries at runtime:
```bash
./bin/ibmw run ./cfg/my_config.yaml
```

**App mode** -- A standalone executable with modules compiled in:
```bash
./my_app ./cfg/my_config.yaml
```

**Registration mode** -- Modules registered programmatically in `main()`:
```bash
./my_app_registration_mode
```

### Starting a New Project with ibmw-gen

To bootstrap a new project without writing boilerplate by hand, use
**ibmw-gen**. Define your modules, messages, and services in one
`.ibmw.yaml` file, then generate the full project:

```bash
# 1. Generate project from .ibmw.yaml
python3 src/tools/ibmw_gen/ibmw_gen.py my_project.ibmw.yaml -o my_project

# 2. Build the generated project
source /opt/ros/humble/setup.bash
cd my_project && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
export IBMW_BUILD_DIR="<ibmw-build-dir>"
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 3. Run
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$IBMW_BUILD_DIR/lib:$IBMW_BUILD_DIR:$IBMW_BUILD_DIR/bin:$LD_LIBRARY_PATH"
./my_app cfg/my_app.yaml
```

> **Important:** Set `$IBMW_BUILD_DIR` to your actual IBMW build directory. The `LD_LIBRARY_PATH`
> must include `build/lib`, `build/`, and `build/bin` because IBMW installs
> shared libraries and executables into these subdirectories.

ibmw-gen produces module scaffolds, CMake rules, YAML configs, and
DDS/JSON type definitions. Fill in the TODO markers with your business
logic.

For a hands-on walkthrough with 8 working demos plus an extension-loading example, see
[ibmw-template/](../../ibmw-template/) and the
[ibmw-gen reference](../../src/tools/ibmw_gen/README.md).

---

## Build Options

All build options are CMake variables set with `-D`:

### Core Options

| Option | Default | Description |
|--------|---------|-------------|
| `IBMW_BUILD_RUNTIME` | ON | Build the core runtime engine |
| `IBMW_BUILD_EXAMPLES` | OFF | Build example applications |
| `IBMW_BUILD_TESTS` | OFF | Build unit tests |
| `IBMW_BUILD_CLI_TOOLS` | OFF | Build the `ibmw` CLI tool |
| `IBMW_BUILD_WITH_ROS2` | OFF | Enable ROS2 rcl integration **(deprecated)** |
| `IBMW_BUILD_PROTOCOLS` | ON | Build DDS IDL protocol definitions |
| `IBMW_USE_FMT_LIB` | ON | Use the fmt library for formatting |
| `IBMW_ENABLE_DLOPEN_DEEPBIND` | ON | Use RTLD_DEEPBIND for plugin loading |

### Extension Options

All extensions default to OFF and require `IBMW_BUILD_RUNTIME=ON`:

| Option | Extension |
|--------|-----------|
| `IBMW_BUILD_DDS_EXTENSION` | DDS transport + RPC (FastDDS) |
| `IBMW_BUILD_NET_EXTENSION` | HTTP/TCP/UDP transport + RPC |
| `IBMW_BUILD_MQTT_EXTENSION` | MQTT transport |
| `IBMW_BUILD_ICEORYX_EXTENSION` | Iceoryx shared-memory transport (zero-copy) |
| `IBMW_BUILD_GRPC_EXTENSION` | gRPC RPC |
| `IBMW_BUILD_ROS2_EXTENSION` | ROS2 transport **(deprecated)** -- use DDS extension instead |
| `IBMW_BUILD_ECHO_EXTENSION` | Echo RPC (testing) |
| `IBMW_BUILD_PROXY_EXTENSION` | Topic proxy/forwarding |
| `IBMW_BUILD_PARAMETER_EXTENSION` | Parameter service |
| `IBMW_BUILD_LOG_CONTROL_EXTENSION` | Runtime log level control |
| `IBMW_BUILD_RECORD_PLAYBACK_EXTENSION` | Message recording/playback |
| `IBMW_BUILD_TIME_MANIPULATOR_EXTENSION` | Time manipulation |
| `IBMW_BUILD_TOPIC_LOGGER_EXTENSION` | Topic-based log output |
| `IBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION` | Service discovery |

---

## Running Demos

### General Setup

```bash
source /opt/ros/humble/setup.bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:$LD_LIBRARY_PATH"
```

### Using Shell Scripts

Most demos provide shell scripts in `build/`:

```bash
bash start_examples_cpp_helloworld.sh
bash start_examples_cpp_executor.sh
bash start_pubsub_local.sh
```

### Using the ibmw Binary (Pkg Mode)

```bash
./bin/ibmw run ./cfg/<config_name>.yaml
```

### Using Standalone Apps (App Mode)

```bash
./<app_name> ./cfg/<config_name>.yaml
```

---

## Demo Reference

### 1. quickstart -- Hello World

**Purpose:** Minimal module lifecycle (`Setup` -> `Run` -> `Stop`).

**What it demonstrates:**
- Module lifecycle hooks
- Per-module YAML configuration access
- `MW_INFO()` logging macro
- All three execution modes (Pkg, App, Registration)

**How to run:**
```bash
# Pkg mode
./bin/ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml

# App mode
./helloworld_app ./cfg/examples_cpp_helloworld_app_mode_cfg.yaml

# Registration mode (no config needed)
./helloworld_app_registration_mode
```

**Required extensions:** None (core only).

---

### 2. executor_demo -- Task Scheduling

**Purpose:** Demonstrates the executor framework: thread pools, strand executors,
timed dispatch, timers, coroutines, and real-time scheduling.

**What it demonstrates:**
- `core_.GetExecutor().GetExecutor("name")` -- obtaining executor handles
- `executor.Dispatch(task)` / `executor.DispatchAfter(duration, task)`
- Strand executor for serialized (thread-safe) execution
- `ibmw::executor::CreateTimer(executor, interval, callback)` -- recurring timers
- `co_await co::Schedule(executor)` -- coroutine scheduling
- Real-time: `SCHED_FIFO`, `SCHED_RR` policies with CPU binding

**How to run:**
```bash
bash start_examples_cpp_executor.sh           # Basic
bash start_examples_cpp_executor_timer.sh     # Timer
bash start_examples_cpp_executor_co.sh        # Coroutines
bash start_examples_cpp_executor_co_loop.sh   # Coroutine loop
bash start_examples_cpp_executor_real_time.sh # Real-time (requires privileges)
```

**Required extensions:** None (core only).

---

### 3. logging_demo -- Logging System

**Purpose:** Demonstrates all log levels, custom formatting, file rotation,
synchronous writes, and benchmarks.

**What it demonstrates:**
- `MW_TRACE/MW_DEBUG/MW_INFO/MW_WARN/MW_ERROR/MW_FATAL()` macros
- Console and file log backends
- Log file rotation by size
- Per-module log levels
- Log throughput benchmarking

**How to run:**
```bash
bash start_examples_cpp_logger.sh                       # All levels
bash start_examples_cpp_logger_format.sh                # Custom format
bash start_examples_cpp_logger_rotate_file.sh           # File rotation
bash start_examples_cpp_logger_rotate_file_with_sync.sh # Sync writes
bash start_examples_cpp_logger_bench.sh                 # Benchmark
```

**Required extensions:** None (core only).

---

### 4. config_param_demo -- Parameters

**Purpose:** Demonstrates the runtime parameter API for key-value storage.

**What it demonstrates:**
- `core_.GetParams()` -- parameter handle
- `SetParameter(key, value)` / `GetParameter(key)`
- Concurrent parameter access from multiple threads

**How to run:**
```bash
bash start_examples_cpp_parameter.sh
```

**Required extensions:** None (core only).

---

### 5. context_demo -- Context API

**Purpose:** Demonstrates the high-level Context API for resource-managed module
development with transport, RPC, logging, and executor access.

**What it demonstrates:**
- `ibmw::context::Context(core)` -- creating a context
- `ctx_ptr_->LetMe()` -- thread-local context ownership
- Publisher/Subscriber through context (inline and on-executor callbacks)
- RPC Client/Server through context
- `ibmw::context::Running()` -- lifecycle-aware loop control

**How to run (8 sub-demos):**
```bash
bash start_examples_cpp_context_executor.sh
bash start_examples_cpp_context_logger.sh

# Transport (run publisher + subscriber in separate terminals)
bash start_examples_cpp_context_publisher.sh
bash start_examples_cpp_context_subscriber_inline.sh

# RPC (run client + server in separate terminals)
bash start_examples_cpp_context_rpc_client.sh
bash start_examples_cpp_context_rpc_server_inline.sh
```

**Required extensions:** None (local transport).

---

### 6. transport_demo -- Pub/Sub

**Purpose:** Demonstrates publish/subscribe messaging with local and DDS backends.

**What it demonstrates:**
- `core_.GetTransport().GetPublisher("topic")` / `GetSubscriber("topic")`
- `ibmw::transport::RegisterSendType<T>()` / `ibmw::transport::Send()`
- `ibmw::transport::OnMessage<T>(subscriber, callback)`
- Local vs DDS backend routing
- Cross-process DDS communication

**How to run:**
```bash
# Local backend (single process)
bash start_pubsub_local.sh

# DDS backend (single process)
bash start_pubsub_dds.sh

# DDS backend (two processes)
bash start_publisher_dds.sh   # Terminal 1
bash start_subscriber_dds.sh  # Terminal 2
```

**Required extensions:** DDS (for DDS demos); none for local.

---

### 7. dds_native_demo -- Raw DDS Types

**Purpose:** Direct DDS pub/sub with hand-written types (no IDL code generation).

**What it demonstrates:**
- Custom DDS type with `IBMW_DDS_TYPE_NAME()` macro
- Hand-written FastCDR serialize/deserialize functions
- Single-process DDS loopback

**How to run:**
```bash
./dds_native_app ./dds_native_cfg.yaml
```

**Required extensions:** DDS.

---

### 8. dds_image_demo -- High-Throughput Messaging

**Purpose:** Demonstrates large-payload DDS messaging (640x480 RGB images at 10Hz).

**What it demonstrates:**
- ~1MB per message, ~10MB/s sustained throughput
- Sub-millisecond latency for large payloads
- Synthetic RGB gradient test patterns

**How to run:**
```bash
./dds_image_app ./dds_image_cfg.yaml
```

**Required extensions:** DDS.

---

### 9. loaned_message_demo -- Zero-Copy

**Purpose:** End-to-end validation of the LoanedMessage API for zero-copy publishing.

**What it demonstrates:**
- `BorrowLoanedMessage<T>()` / `SendLoaned()` -- zero-copy publishing
- Plain types (zero-copy via SHM) vs non-Plain types (heap fallback)
- Three configuration modes: AUTO, STRICT, COMPAT
- Zero-copy statistics tracking

**How to run:**
```bash
# Full end-to-end test
./loaned_message_app ./cfg/loaned_message_cfg.yaml

# Configuration mode test (6 automated scenarios)
./config_test_app ./cfg/config_test_cfg.yaml
```

**Required extensions:** DDS + ROS2 (rosidl build-time only; no rcl runtime).

> **Note:** Requires `full` CMake preset (or higher) with `IBMW_BUILD_ROS2_TYPES=ON`.
> Runs without rcl at runtime (pure FastDDS transport).

---

### 10. ros2_chn -- ROS2 Transport **(deprecated)**

**Purpose:** Demonstrates ROS2 message-based transport pub/sub using the local backend.

**What it demonstrates:**
- ROS2 `.msg` protocol types (`example_ros2::msg::RosTestMsg`)
- `ibmw::transport::RegisterSendType<T>()` with ROS2 types
- Pkg mode (two packages) and App mode variants

**How to run:**
```bash
bash start_examples_cpp_ros2_chn.sh               # Two packages
bash start_examples_cpp_ros2_chn_single_pkg.sh     # Single package
bash start_examples_cpp_ros2_chn_publisher_app.sh  # App-mode publisher
bash start_examples_cpp_ros2_chn_subscriber_app.sh # App-mode subscriber
```

**Required extensions:** ROS2 (local backend).

> **Deprecated:** `ros2_chn` requires the `ros2_extension` which depends on rcl
> at runtime. Prefer the DDS extension (`dds_ext`) for new projects. Use the
> `ros2` CMake preset to build this demo.

---

### 11. ros2_rpc -- RPC Patterns **(deprecated — use dds_rpc_demo instead)**

**Purpose:** Comprehensive RPC demonstration with 7 variants covering all calling
patterns.

**What it demonstrates:**
- Synchronous RPC: `SyncAccess` -- blocking call with timeout
- Asynchronous RPC: `AsyncAccess` -- callback-based
- Future RPC: `FutureAccess` -- `std::future`-based
- Coroutine RPC: `CoAccess` -- `co_await`-based
- RPC filters/middleware for logging and timing
- Per-call timeout via `ctx_ptr->SetTimeout(3s)`

**How to run:**
```bash
bash start_examples_cpp_ros2_rpc_sync.sh       # Sync
bash start_examples_cpp_ros2_rpc_async.sh      # Async
bash start_examples_cpp_ros2_rpc_future.sh     # Future
bash start_examples_cpp_ros2_rpc_co.sh         # Coroutine + filters
bash start_examples_cpp_ros2_rpc_single_pkg.sh # Single package
bash start_examples_cpp_ros2_rpc_server_app.sh # App-mode server
bash start_examples_cpp_ros2_rpc_client_app.sh # App-mode client
```

**Required extensions:** ROS2 (local RPC).

> **Deprecated:** `ros2_rpc` uses `type: ros2` service driver which requires rcl
> at runtime. Use `type: dds` service driver and see `dds_rpc_demo` for the
> recommended pure-DDS alternative. Use the `ros2` CMake preset to build this demo.

---

### 12. ros2_interop_demo -- ROS2 Interoperability

**Purpose:** Demonstrates bidirectional communication with ROS2 nodes via FastDDS
without any RCL/RMW dependency.

**What it demonstrates:**
- `ibmw::ros2::transport::RegisterSendType<T>()` -- ROS2-compatible type registration
- `ros2_compatible_mode: true` -- ROS2 topic naming with `rt/` prefix
- Bidirectional: `ros2 topic echo /chatter` sees IBMW-published messages
- Standard ROS2 types (String, Int32, Point, Pose, Twist)

**How to run:**
```bash
# Terminal 1: IBMW node
./ros2_interop_app ./cfg/ros2_interop_cfg.yaml

# Terminal 2: Verify with ROS2 CLI
ros2 topic list
ros2 topic echo /chatter

# Terminal 3: ROS2 talker
ros2 run demo_nodes_cpp talker
```

**Required extensions:** DDS + ROS2 (rosidl build-time only; no rcl runtime).

> **Note:** Requires `full` CMake preset (or higher) with `IBMW_BUILD_ROS2_TYPES=ON`.
> Runs without rcl at runtime (pure FastDDS transport).

---

### 13. ros2_point_demo -- Zero-Copy Plain Types

**Purpose:** Demonstrates zero-copy communication with ROS2 Plain types
(`geometry_msgs::msg::Point`).

**What it demonstrates:**
- Plain type detection (`trivially_copyable` + `standard_layout`)
- Automatic zero-copy optimization via DDS `loan_sample`
- ROS2 interoperability: `ros2 topic echo /rt/point_topic`

**How to run:**
```bash
./ros2_point_app ./cfg/ros2_point_cfg.yaml

# Verify in another terminal:
ros2 topic echo /rt/point_topic geometry_msgs/msg/Point
```

**Required extensions:** DDS + ROS2 (rosidl build-time only; no rcl runtime).

> **Note:** Requires `full` CMake preset (or higher) with `IBMW_BUILD_ROS2_TYPES=ON`.
> Runs without rcl at runtime (pure FastDDS transport).

---

### 14. dds_rpc_demo -- Pure-DDS RPC (Native .srv, ROS2-Compatible)

**Purpose:** Demonstrates the DDS RPC service driver (`type: dds`) using native
`.srv` mode for cross-process RPC over FastDDS. Business types are directly
serialized/deserialized by FastDDS (no envelope wrappers), enabling 100%
wire-compatible interop with ROS2 standard services.

**What it demonstrates:**
- `type: dds` service driver with native `.srv` TypeSupport
- Pure-DDS RPC -- no rcl/rclcpp needed, just FastDDS
- 100% ROS2 wire-compatible: `ros2 service call` and `ros2 service list` work directly
- `srv:` field in ibmw-gen for automatic ROS2 DDS type name derivation
- Bidirectional ROS2 interop (IBMW server ↔ ROS2 client, ROS2 server ↔ IBMW client)
- Coroutine-based (`co_await`) RPC client and server patterns
- ibmw-gen end-to-end code generation for DDS RPC projects

**How to run:**
```bash
# Generate from .ibmw.yaml
cd src/samples/cpp/dds_rpc_demo
python3 <ibmw>/src/tools/ibmw_gen/ibmw_gen.py dds_rpc_demo.ibmw.yaml -o dds_rpc_demo_gen

# Build
cd dds_rpc_demo_gen && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
cmake .. -DCMAKE_PREFIX_PATH=<ibmw-install-dir>
make -j$(nproc)

# Set up runtime environment
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:<ibmw-install-dir>/lib:<ibmw-install-dir>:<ibmw-install-dir>/bin:$LD_LIBRARY_PATH"

# Run (two terminals)
./dds_rpc_server_app cfg/dds_rpc_server_app.yaml   # Terminal 1
./dds_rpc_client_app cfg/dds_rpc_client_app.yaml   # Terminal 2

# Verify ROS2 sees the service (optional)
ros2 service list

# ROS2 interop: call IBMW server from ROS2
ros2 service call /add_two_ints example_interfaces/srv/AddTwoInts "{a: 3, b: 5}"

# ROS2 interop: IBMW client calls a ROS2 server
ros2 run demo_nodes_cpp add_two_ints_server   # Terminal 3
# Then run the IBMW client -- it will call the ROS2 server
```

**Required extensions:** DDS.

---

### 15. iceoryx_inproc_demo -- Iceoryx Single-Process Pub/Sub

**Purpose:** Demonstrates the Iceoryx shared-memory transport in a single
process, exercising automatic SHM expansion with messages from 1 KB to 1 MB.

**What it demonstrates:**
- `IBMW_BUILD_ICEORYX_EXTENSION` build path
- `iceoryx_extension` + `type: iceoryx` transport driver loaded together
- IDL type registered via `IBMW_DDS_TYPE_NAME` + `RegisterSendType<T>`
- Driver-agnostic byte transfer (the iceoryx driver reuses `dds_types`
  CDR serialization, not FastDDS at runtime)
- Automatic SHM segment doubling when payload outgrows `shm_init_size`
- RouDi daemon (`iox-roudi`) is **required**, even single-process

**How to run:**
```bash
# Build (tests optional)
source /opt/ros/humble/setup.bash
cmake -B build -DIBMW_BUILD_ICEORYX_EXTENSION=ON
cmake --build build -j$(nproc) --target iceoryx_inproc_app iox-roudi \
                                        ibmw_samples_cpp_iceoryx_inproc_demo_build_all

# Run (single terminal -- start_*.sh manages RouDi for you)
cd build
bash start_iceoryx_inproc.sh
```

**Required extensions:** Iceoryx (`IBMW_BUILD_ICEORYX_EXTENSION=ON`).

> **Tip:** `shm_init_size` in the YAML is intentionally tiny (256 bytes) so
> that 1 MB payloads exercise the `LoanShm` doubling loop. Iceoryx's own SHM
> growth logs are at Debug level; successful end-to-end delivery of 1 MB
> samples is sufficient evidence the loop works.

---

### 16. iceoryx_interproc_demo -- Iceoryx Two-Process Pub/Sub

**Purpose:** Same payload mix as `iceoryx_inproc_demo`, but publisher and
subscriber run in separate processes communicating through a single shared
RouDi daemon.

**What it demonstrates:**
- Two IBMW runtimes with **distinct `runtime_id`** sharing one RouDi
- Subscriber must register its Listener **before** the publisher's first
  send (iceoryx history depth defaults are small)
- Cross-process zero-copy: 1 MB payload end-to-end RTT remains in the
  ~1.5 ms range -- only the SHM offset crosses the process boundary

**How to run:**
```bash
# Build
source /opt/ros/humble/setup.bash
cmake -B build -DIBMW_BUILD_ICEORYX_EXTENSION=ON
cmake --build build -j$(nproc) \
    --target iceoryx_interproc_publisher_app iceoryx_interproc_subscriber_app \
             iox-roudi ibmw_samples_cpp_iceoryx_interproc_demo_build_all

# Run via the helper script (auto-starts RouDi, then sub, then pub)
cd build
DURATION=15 bash start_iceoryx_interproc.sh

# Or run manually in three terminals:
./iox-roudi                                                # Terminal 1
./iceoryx_interproc_subscriber_app cfg/subscriber.yaml     # Terminal 2 (start first)
./iceoryx_interproc_publisher_app cfg/publisher.yaml       # Terminal 3
```

**Required extensions:** Iceoryx (`IBMW_BUILD_ICEORYX_EXTENSION=ON`).

---

### 17. multi_driver_demo -- One Module, Multiple Drivers

**Purpose:** Demonstrates the variadic `IBMW_USE_DRIVERS(...)` macro,
binding **two** independent transport drivers (DDS + Iceoryx) to typed
slots inside a **single** `Module` class. This shows that the typed
driver-slot ABI composes cleanly across extensions.

**What it shows:**
- One user-region module holds both `DdsTransportDriver*` and
  `IceoryxTransportDriver*` member pointers
- A single `IBMW_USE_DRIVERS(IBMW_DRIVER_ENTRY(...), IBMW_DRIVER_ENTRY(...))`
  declaration covers both drivers (the single-driver helper is
  `IBMW_USE_DRIVER`)
- Covers all four module flavours simultaneously: managed source, standalone
  app binary, pkg `.so`, and ibmw-gen regenerated tree (byte-equal)

**How to run:**
```bash
cd build

# App mode (no live transport extension required)
./multi_driver_demo_app ./cfg/multi_driver_demo_app_mode_cfg.yaml

# Pkg mode
./bin/ibmw run ./cfg/multi_driver_demo_cfg.yaml
```

**Required extensions:** DDS (`IBMW_BUILD_DDS_EXTENSION=ON`) and
Iceoryx (`IBMW_BUILD_ICEORYX_EXTENSION=ON`); both are enabled by the
`full` preset.

**Minimal code skeleton:**
```cpp
class MultiDriverModule : public ibmw::Module {
 private:
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* iox_drv_ = nullptr;

  // One IBMW_USE_DRIVERS binds both drivers to this module.
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver,
                        dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxTransportDriver,
                        iox_drv_))
};
```

> See `src/samples/cpp/multi_driver_demo/README.md` for the full source
> walkthrough.

---

All IBMW configuration lives under the `ibmw:` top-level key. Module-specific
configuration uses separate top-level keys matching the module name.

Environment variables are supported: `${MY_VAR}` is substituted before parsing.

### Top-Level Structure

```yaml
ibmw:
  extensions: ...      # Extension/plugin loading
  main_loop: ...       # Main thread executor
  watchdog: ...        # Watchdog thread
  scheduling: ...      # User-defined executors
  logging: ...         # Log backends and levels
  memory: ...          # Allocator config (reserved)
  service: ...          # Service (RPC) drivers and routing
  messaging: ...       # Transport drivers and routing
  parameter: ...       # Parameter store (reserved)
  modules: ...         # Package loading, per-module config
  config: ...           # Config subsystem options

MyModule: ...          # Custom config for "MyModule"
```

### Subsystem Initialization Order

Extensions -> Main Loop -> Watchdog -> Executors -> Logging -> Memory ->
RPC -> Transport -> Parameters -> Modules -> Configurator

---

### `ibmw.extensions`

```yaml
ibmw:
  extensions:
    items:
      - name: <string>       # Unique extension name (required)
        path: <string>       # Path to .so library (required)
        options: { ... }     # Extension-specific config (optional)
```

---

### `ibmw.main_loop`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | `"main_loop"` | Executor name |
| `thread_sched_policy` | string | `""` | OS scheduling policy (e.g., `"SCHED_FIFO:50"`) |
| `thread_bind_cpu` | uint32[] | `[]` | CPU affinity list |

---

### `ibmw.watchdog`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `name` | string | `"watchdog"` | Executor name |
| `thread_sched_policy` | string | `""` | OS scheduling policy |
| `thread_bind_cpu` | uint32[] | `[]` | CPU affinity list |
| `queue_threshold` | uint32 | `0` | Queue depth alarm threshold (0=disabled) |
| `threshold_alarm_interval_ms` | int32 | `0` | Min interval between alarms (ms) |

---

### `ibmw.scheduling`

Array of executor definitions:

```yaml
ibmw:
  scheduling:
    - name: <string>      # Unique name (required)
      type: <string>      # Executor type (required)
      options: { ... }    # Type-specific options
```

#### `thread_pool` options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `thread_num` | uint32 | `1` | Number of threads |
| `thread_sched_policy` | string | `""` | OS scheduling policy |
| `thread_bind_cpu` | uint32[] | `[]` | CPU affinity |
| `timeout_alarm_threshold_us` | uint64 | `0` | Task timeout alarm (us, 0=disabled) |
| `use_system_clock` | bool | `false` | Use system clock for timeouts |

#### `dedicated` options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `thread_sched_policy` | string | `""` | OS scheduling policy |
| `thread_bind_cpu` | uint32[] | `[]` | CPU affinity |
| `queue_threshold` | uint32 | `0` | Queue depth alarm (0=disabled) |

#### `strand` options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `bind_thread_pool_executor_name` | string | -- | **Required.** Thread pool to bind to |
| `timeout_alarm_threshold_us` | uint64 | `0` | Task timeout alarm (us) |

#### `timer` options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `bind_executor` | string | `""` | Executor to bind to (empty=own thread) |
| `dt_us` | uint64 | `1000` | Tick resolution (us) |
| `wheel_size` | size_t[] | `[1000]` | Time wheel level sizes |
| `thread_sched_policy` | string | `""` | OS scheduling policy (own thread only) |
| `thread_bind_cpu` | uint32[] | `[]` | CPU affinity (own thread only) |

#### `parallel` options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `thread_num` | uint32 | `1` | Number of TBB threads |
| `thread_sched_policy` | string | `""` | OS scheduling policy |
| `thread_bind_cpu` | uint32[] | `[]` | CPU affinity |
| `queue_threshold` | uint32 | `0` | Queue depth alarm (0=disabled) |

---

### `ibmw.logging`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `core_level` | string | `"Info"` | Framework log level: Trace/Debug/Info/Warn/Error/Fatal/Off |
| `default_module_level` | string | `"Info"` | Default module log level |
| `enable_crash_log` | bool | `true` | Enable signal handler for crash logging |
| `backends` | array | `[]` | Log backend configurations |

#### Console backend (`type: console`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `color` | bool | `true` | ANSI color output |
| `module_filter` | string | `""` | Regex filter on module names |
| `pattern` | string | `""` | Custom log format pattern |
| `log_executor_name` | string | `""` | Executor for async logging |

#### Rotate file backend (`type: rotate_file`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `path` | string | `""` | Log file directory |
| `filename` | string | `""` | Base filename |
| `max_file_size_m` | uint32 | `10` | Max file size (MB) |
| `max_file_num` | uint32 | `5` | Max rotated files |
| `module_filter` | string | `""` | Regex filter |
| `pattern` | string | `""` | Custom format pattern |
| `enable_sync` | bool | `false` | Periodic flush |
| `sync_interval_ms` | uint32 | `1000` | Flush interval (ms) |
| `sync_executor_name` | string | `""` | Executor for flush task |
| `suffix_with_timestamp` | bool | `false` | Append timestamp to filename |

---

### `ibmw.service`

```yaml
ibmw:
  service:
    drivers:
      - type: <string>       # local, dds, http, grpc
        options: { ... }
    clients:
      - func_name: <regex>   # Match RPC function names
        enable_drivers: [...]
        enable_filters: [...]
    servers:
      - func_name: <regex>
        enable_drivers: [...]
        enable_filters: [...]
```

#### Local RPC driver (`type: local`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `timeout_executor` | string | `""` | Executor for client timeout handling |
| `timeout_handle_executor` | string | `""` | Executor for server timeout handling |

#### DDS RPC driver (`type: dds`)

The DDS RPC driver enables cross-process RPC over FastDDS without any
rcl/rclcpp dependency. It uses **native `.srv` mode**: business request/response
types are directly serialized by FastDDS using the TypeSupport from gencode
(no envelope wrappers). This makes the wire format 100% compatible with ROS2
standard services.

When `ros2_compatible` is true (the default when loaded from the DDS extension),
it uses ROS2 service wire protocol conventions (`rq/`/`rr/` topic prefixes,
`dds_::` type name infixes, URL-encoded names), making the service discoverable
and callable by standard ROS2 nodes (`ros2 service call`, `ros2 service list`).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `ros2_compatible` | bool | `true` | Use ROS2 naming conventions for DDS topics |

**How it works:** Each RPC function maps to a request topic
(`rq/<service_name>Request`) and a reply topic (`rr/<service_name>Reply`).
The DDS type names and CDR payloads are derived from the `.srv` definition
(e.g., `example_interfaces::srv::dds_::AddTwoInts_Request_`), matching exactly
what ROS2 uses. Request/response correlation uses the RTPS `WriteParams` /
`SampleInfo.related_sample_identity` mechanism -- the same mechanism used by
`rmw_fastrtps` internally.

**Routing example:**

```yaml
ibmw:
  service:
    drivers:
      - type: dds
        options:
          ros2_compatible: true
      - type: local
    clients:
      - func_name: "dds:(.*)"     # Route dds:-prefixed functions to DDS
        enable_drivers: [dds]
      - func_name: "(.*)"         # Everything else stays in-process
        enable_drivers: [local]
    servers:
      - func_name: "dds:(.*)"
        enable_drivers: [dds]
      - func_name: "(.*)"
        enable_drivers: [local]
```

---

### `ibmw.messaging`

```yaml
ibmw:
  messaging:
    drivers:
      - type: <string>       # local, dds, mqtt, http, tcp, udp, iceoryx
        options: { ... }
    publishers:
      - topic: <regex>       # Match topic names
        enable_drivers: [...]
        enable_filters: [...]
    subscribers:
      - topic: <regex>
        enable_drivers: [...]
        enable_filters: [...]
```

#### Local transport driver (`type: local`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `subscriber_use_inline_executor` | bool | `false` | Run callbacks in publisher thread |
| `subscriber_executor` | string | `""` | Default subscriber callback executor |

---

### `ibmw.modules`

```yaml
ibmw:
  modules:
    packages:
      - path: <string>                 # Path to .so (required)
        enable_modules: [<string>]     # Whitelist (optional)
        disable_modules: [<string>]    # Blacklist (optional)
    modules:
      - name: <string>                 # Module name (required)
        log_level: <string>            # Override log level
        enable: <bool>                 # Enable/disable module
        cfg_file_path: <string>        # Path to module config file
```

---

### Extension-Specific Options

#### DDS Extension

Extension options:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `domain_id` | uint32 | `0` | DDS domain ID |
| `participant_name` | string | `""` | DDS participant name |

DDS transport driver options (`type: dds` under `messaging`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `ros2_compatible` | bool | `false` | ROS2-compatible topic naming |
| `ros2_topic_prefix` | string | `"rt"` | ROS2 topic prefix |
| `publishers[].topic` | string | -- | Topic name |
| `publishers[].qos.history` | string | `"keep_last"` | `keep_last` or `keep_all` |
| `publishers[].qos.depth` | int | `10` | History depth |
| `publishers[].qos.reliability` | string | `"reliable"` | `reliable` or `best_effort` |
| `publishers[].qos.durability` | string | `"volatile"` | `volatile` or `transient_local` |
| `subscribers[].topic` | string | -- | Topic name or regex |
| `subscribers[].qos.history` | string | `"keep_last"` | `keep_last` or `keep_all` |
| `subscribers[].qos.depth` | int | `10` | History depth |
| `subscribers[].qos.reliability` | string | `"reliable"` | `reliable` or `best_effort` |
| `subscribers[].qos.durability` | string | `"volatile"` | `volatile` or `transient_local` |
| `subscribers[].executor` | string | `""` | Callback executor. An unknown name is a configuration error. |
| `subscribers[].loan_mode` | string | `"auto"` | `auto`, `sync`, or `async`. `auto` runs synchronously unless `executor` is set; with `executor` it dispatches asynchronously. `async` requires `executor`. `sync` ignores `executor`. |
| `zero_copy.mode` | string | `"auto"` | `auto`, `strict`, `compat` |

When DDS subscriber callbacks are dispatched through an executor, loaned DDS
samples stay checked out until the executor task completes. Use a single-threaded
executor when per-topic callback order matters, and size DDS history/loan pools
for the callback hold time.

Reliable `keep_last` publishers use 100 extra writer cache slots by default to
favor functional reliability for common robotics and SLAM workloads. Most users
do not need to configure this. Memory-sensitive deployments can set
`IBMW_DDS_RELIABLE_EXTRA_SAMPLES` to an integer from `0` through `INT32_MAX`, or
`-1` to restore the Fast DDS default. Invalid values fall back to the safe
default of 100. This does not increase the logical history depth and is not
applied to readers, best-effort writers, or `keep_all` writers.

#### gRPC Extension

Extension options:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `thread_num` | uint32 | `1` | gRPC thread count |
| `listen_ip` | string | `"0.0.0.0"` | Listen address |
| `listen_port` | uint16 | `0` | Listen port |

gRPC RPC driver options (`type: grpc`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `clients[].func_name` | string | -- | RPC function name |
| `clients[].server_url` | string | -- | Remote server URL |
| `servers[].func_name` | string | -- | RPC function name |

#### Net (HTTP/TCP/UDP) Extension

Extension options:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `thread_num` | uint32 | -- | Network I/O threads (required) |
| `http_options.listen_ip` | string | `"0.0.0.0"` | HTTP listen IP |
| `http_options.listen_port` | uint16 | `0` | HTTP listen port |
| `tcp_options.listen_ip` | string | `"0.0.0.0"` | TCP listen IP |
| `tcp_options.listen_port` | uint16 | `0` | TCP listen port |
| `udp_options.listen_ip` | string | `"0.0.0.0"` | UDP listen IP |
| `udp_options.listen_port` | uint16 | `0` | UDP listen port |
| `udp_options.max_pkg_size` | uint32 | `65535` | Max UDP datagram |

#### MQTT Extension

Extension options:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `broker_addr` | string | -- | Broker address (required) |
| `client_id` | string | -- | Client ID (required) |
| `max_pkg_size_k` | uint32 | `0` | Max package size KB |
| `reconnect_interval_ms` | uint32 | `5000` | Reconnect interval |
| `truststore` | string | `""` | CA cert path (TLS) |
| `client_cert` | string | `""` | Client cert (TLS) |
| `client_key` | string | `""` | Client key (TLS) |
| `callback_executor_name` | string | `""` | Callback executor |

MQTT transport driver (`type: mqtt`) per-topic QoS: 0, 1, or 2.

#### Iceoryx Extension

Shared-memory zero-copy transport. Requires the `iox-roudi` daemon to be
running before any IBMW process using the iceoryx driver starts.

Extension options (`name: iceoryx_extension`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `shm_init_size` | uint64 | `1024` | Initial loaned SHM size in bytes. Doubled automatically by `LoanShm`/`UpdateLoanShm` when a payload does not fit. |
| `runtime_id` | string | `"iceoryx<pid>"` | Iceoryx PoshRuntime identifier. Must be unique across processes sharing the same RouDi. |

Iceoryx transport driver options (`type: iceoryx` under `messaging`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `listener_thread_name` | string | `""` | Name applied to the subscriber Listener thread (visible in `ps -L`/`htop`). |
| `listener_thread_sched_policy` | string | `""` | OS scheduling policy, e.g. `"SCHED_FIFO:50"`. |
| `listener_thread_bind_cpu` | uint32[] | `[]` | CPU affinity list for the Listener thread. |

Topic-to-iceoryx URL mapping: each registered IBMW topic becomes the
3-segment iceoryx `ServiceDescription`
`/transport / <UrlEncode(topic_name)> / <UrlEncode(msg_type)>`.
Each segment is truncated to `iox::MAX_RUNTIME_NAME_LENGTH` characters with
a `...` ellipsis if longer (a warning is logged).

Minimal example (single process):

```yaml
ibmw:
  extensions:
    items:
      - name: iceoryx_extension
        path: ./lib/libibmw_iceoryx_transport.so
        options:
          shm_init_size: 256        # tiny, exercises auto-expansion
          runtime_id: my_iox_app
  messaging:
    drivers:
      - type: iceoryx
        options:
          listener_thread_name: iox_listen
    publishers:
      - topic: "(.*)"
        enable_drivers: [iceoryx]
    subscribers:
      - topic: "(.*)"
        enable_drivers: [iceoryx]
```

For two-process pub/sub, give each process a **different** `runtime_id`
(both pointing at the same RouDi) and start the subscriber **before** the
publisher's first send.

#### ROS2 Extension

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `node_name` | string | -- | ROS2 node name (required) |
| `executor_type` | string | `"SingleThreaded"` | ROS2 executor type |
| `executor_thread_num` | uint32 | `1` | Thread count (MultiThreaded) |

#### Record/Playback Extension

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `service_name` | string | `""` | RPC service name |
| `timer_executor` | string | `""` | Timer executor |
| `record_actions[].name` | string | -- | Action name |
| `record_actions[].options.bag_path` | string | -- | Bag storage dir |
| `record_actions[].options.mode` | string | -- | `imd` or `signal` |
| `record_actions[].options.topic_meta_list[].topic` | string | -- | Topic |
| `record_actions[].options.topic_meta_list[].msg_type` | string | -- | Type |

Runtime Graph (`ibmw.runtime_graph`) owns runtime advertisements, topic discovery,
and the control socket. Configure TypeSupport packages used for topic schemas,
recording, or playback under `ibmw.runtime_graph.type_support_pkgs`; the
Record/Playback extension does not maintain a separate copy.

#### Topic CLI

`ibmw topic hz <topic>` measures the rate received by an observer. Once per second
it prints average Hz, minimum/maximum interarrival time, population standard
deviation, and the number of messages in the window. It uses a steady clock and
the latest 10000 messages by default. ROS simulation time and message field
filters are not currently supported.

```bash
ibmw topic hz /sensor_data                     # Registry discovery, capability-based subscription
ibmw topic hz /chatter -r --ros-domain 10       # Independent ROS 2 subscriber
ibmw topic hz rt/chatter -d --dds-domain 10     # Native DDS wire topic name
ibmw topic hz /chatter -z --zenoh-domain robot --zenoh-config ./observer.json5
ibmw topic hz /chatter -r -w 100 --duration 10
```

`hz` accepts one discovery source per invocation and does not support `--all`.
Use `--type <type>` with a name reported by `topic type` to select among multiple
published types. Explicit queries include hidden topics. `--window/-w` must be
at least 2. `--timeout` bounds the wait to discover a publisher (default: 10
seconds); `--duration` limits observation after subscription, otherwise Ctrl+C
stops the command. Silent publishers produce a no-new-messages notice. The
measured rate depends on observer scheduling and transport loss and does not
represent a business subscriber's received rate.

Network observation loads the selected transport library from the installation
directory. ROS 2 generic subscriptions still require the corresponding message
type package to be installed. Optional `--config-file <yaml>` loads only the
selected `ibmw.extensions.items`, `ibmw.messaging.drivers`, and scheduling
configuration. Relative paths are resolved against that file; discovery options
and their defaults always determine the domain. `--zenoh-config` takes precedence
over the file's `native_cfg_path`. DDS/ROS 2 request `best_effort` and `volatile`
by default, with `--qos-reliability` and `--qos-durability` overrides. For DDS
publishers with exclusive ownership, unspecified reliability defaults to `reliable`;
explicit `--qos-reliability best_effort` is rejected as incompatible. Registry
observation follows publisher additions/removals and shares capture route selection
with `bag record --registry`: independent subscriptions first, Runtime Tap as fallback.
DDS/ROS 2 QoS from registry discovery adapts to publishers; cached typed subscriptions retain the
single-publisher and pre-start registration restrictions. Tap drops are reported
when detected.

`ibmw topic list`, `info`, `type`, and `find` share these discovery options:

| Option | Meaning |
|--------|---------|
| `--registry` | Read local runtime registrations across all transport drivers; default source |
| `-d`, `--dds` | Select DDS discovery |
| `-r`, `--ros2` | Select ROS 2 discovery |
| `-z`, `--zenoh` | Select Zenoh discovery |
| `-a`, `--all` | Select all four sources, within their configured domains |
| `--domain <id>` | Set the domain for selected DDS and ROS 2 sources |
| `--dds-domain <id>` | Override the DDS domain; otherwise `--domain` or 0 |
| `--ros-domain <id>` | Override the ROS 2 domain; otherwise `--domain`, `ROS_DOMAIN_ID`, or 0 |
| `--zenoh-domain <name>` | Zenoh business namespace (`limit_domain`); default empty |
| `--zenoh-config <path>` | Zenoh native connection configuration |
| `--registry-path <path>` | Local registry root; default `$XDG_RUNTIME_DIR/ibmw` or `/tmp/ibmw-<euid>`. Protocol data lives in `v2/`; custom roots must be private (0700). |

Verbose output labels this source as `source: registry`; `drivers` lists the actual transports.

Like ROS 2, `info <topic>` directly resolves hidden topics. `list`, `find`, and
`type` exclude them unless `--include-hidden-topics` is set. The switch works
before or after the subcommand, e.g. `ibmw topic --include-hidden-topics type /_hidden`.

`info -v` shows each endpoint's type and available ownership information, including
module names from runtime registrations. QoS is displayed separately for each
driver. Reported policies retain their values; missing policies show `unknown`,
and an empty profile shows `unavailable (not reported by this source)`. Runtime
registrations require the current `module_name` and `driver_qos` fields.
Combined-source counts include observations from each source, without cross-source deduplication.

Source flags can be combined: `list --registry --dds`, `list -d -r --domain 10`.
Domain/configuration options require their corresponding source to be selected.
Numeric domains accept unsigned 32-bit decimal values; backend-specific network
limits will be checked by the respective network adapters.

The DDS source is implemented when `IBMW_BUILD_DDS_EXTENSION` is enabled. The
separate `libibmw_discovery_dds.so` plugin is loaded only when DDS is selected;
source-only local queries do not load it. `IBMW_DISCOVERY_PLUGIN_DIR` can be
used to override the plugin lookup directory.

DDS joins the selected domain and collects native writer and reader discovery
callbacks without subscribing to business data. The result keeps the native DDS
topic and type names, endpoint and participant GUIDs, CDR serialization and
the reported QoS.

The ROS 2 source requires `IBMW_BUILD_WITH_ROS2` and `IBMW_BUILD_ROS2_EXTENSION`.
Selecting `-r` loads the separate `libibmw_discovery_ros2.so` plugin. It queries
the ROS graph through a private rclcpp context and observer node, without
initializing the IBMW ROS 2 business transport extension or subscribing to
business data. The plugin requires the ROS 2/rclcpp runtime and follows native
ROS environment settings such as `RMW_IMPLEMENTATION`. Results contain logical
ROS topic/type names, endpoint GIDs and graph-reported QoS; `topic info -v` also
shows node names and namespaces. Unavailable DDS participant GUIDs and wire
names remain empty rather than being inferred from RMW GIDs or ROS names.
The observer disables rosout, parameter events and services such as parameter
services.

```bash
ibmw topic list -r --ros-domain 10
ibmw topic info /chatter -r --ros-domain 10 -v
ibmw topic list -d -r --dds-domain 10 --ros-domain 20
```

The Zenoh source requires `IBMW_BUILD_ZENOH_EXTENSION`. Selecting `-z` loads
`libibmw_discovery_zenoh.so` and opens its own Zenoh session, without loading the
business transport extension into the CLI. The running business application
must use the updated `libibmw_zenoh_transport.so`: it serves endpoint metadata
through a queryable on its existing session. Arbitrary Zenoh applications and
older IBMW extensions do not expose this metadata automatically.

`--zenoh-domain` must exactly match the business YAML `limit_domain` value;
omitting it selects the empty domain, not every domain. `--zenoh-config` accepts
a native Zenoh configuration for the observer (for example, peer/router
connections); the observer must be able to reach the business sessions. With no
configuration file it uses Zenoh defaults, including native scouting settings.
UDP scouting establishes session connectivity; IBMW metadata queries then
provide the logical topic and type names.

```bash
ibmw topic list -z --zenoh-domain robot --zenoh-config ./observer.json5
ibmw topic info /chatter -z --zenoh-domain robot --zenoh-config ./observer.json5 -v
ibmw topic type /chatter -z --zenoh-domain robot --zenoh-config ./observer.json5
```

Results count successfully declared Zenoh publishers/subscribers, including
silent and subscriber-only topics. Multiple module callbacks sharing one native
subscriber count as one endpoint. RPC internals are excluded. Each endpoint has
its session ZID, instance-local ID, logical topic/type and actual wire key.
Serialization and QoS remain empty when registration does not establish them.
The observer retries metadata queries during the collection window and replaces
each instance's snapshot by revision, removing instances that have not replied
for two seconds. A snapshot remains a bounded observation: an instance may exit
just before the result is printed.

Network queries collect a snapshot over a default two-second window. Each
invocation discovers independently, without a persistent discovery service.
Domain, discovery settings, security and network reachability determine what
can be observed. DDS, ROS 2 and Zenoh remain separate sources: combined queries keep
their endpoints without merging identities across sources. Failed sources return
exit status 1 while successful sources still
print their results.

Native ROS 2 libraries can exit the process during loading, for example when
Humble is given an uninstalled `RMW_IMPLEMENTATION`. This happens before the
adapter runs, so it cannot be caught as a source error or preserve other sources'
results. The selected RMW must be available before using ROS 2 discovery.

`bag record` defaults to local registry discovery and selects subscriptions by
transport capability; network recording supports DDS, ROS 2 and Zenoh:

```bash
ibmw bag record --registry /chatter -o ./registry_bag
ibmw bag record --dds --domain 87 --config-file ./dds-recorder.yaml /native_topic -o ./dds_bag
ibmw bag record --ros2 --domain 87 --config-file ./ros2-recorder.yaml /chatter -o ./ros2_bag
ibmw bag record --zenoh --zenoh-domain robot --config-file ./zenoh-recorder.yaml /chatter -o ./zenoh_bag
```

Registry discovery does not prescribe the capture transport. Registry recording uses
advertised capabilities and connection settings to create an independent observer
runtime, choosing iceoryx, DDS, Zenoh, MQTT, ROS 2, then Runtime Tap in priority
order. DDS, ROS 2 and Zenoh use native serialized subscriptions; iceoryx and MQTT
use TypeSupport-backed cached typed subscriptions. Observer runtimes use distinct
instance names and do not load business modules. One direct route must cover all
publishers of a topic, preventing duplicate recording through multiple drivers.

Cached typed subscriptions require one publisher and registration before driver
startup; existing typed subscriptions can be reused afterward. Multiple publishers,
conflicting connections or unsupported dynamic registration select another eligible
direct route, then one Tap per publisher. UDP, TCP, HTTP and local usually use Tap.
Tap must be advertised by the publisher and does not capture explicit
Loan/PublishLoaned publications that bypass ordinary Send.

Network recording obtains subscription addresses from the selected discovery
plugin and receives serialized messages through the recorder's transport extension,
without publisher registration files. One network subscription receives all
publishers of a topic. Network mode uses recorder-owned configuration and never
loads extensions or TypeSupport paths supplied by network metadata.

Network recording requires recorder-owned configuration and local TypeSupport
packages containing schemas:

```yaml
ibmw:
  runtime_graph:
    type_support_pkgs:
      - path: ./libcommon_dds_ts.so
  extensions:
    items:
      - name: dds_extension
        path: ./libibmw_dds_transport.so
        options: {domain_id: 87}
  messaging:
    drivers:
      - type: dds
        options:
          subscribers:
            - topic: ".*"
              qos: {history: keep_last, depth: 100}
```

For ROS 2, use `ros2_extension`, `libibmw_ros2_transport.so`, `type: ros2`, and
TypeSupport packages for the selected ROS message types. The recorder assigns
its own node name and owns an independent ROS context. For Zenoh, use
`zenoh_extension`, `libibmw_zenoh_transport.so`, `type: zenoh`, with extension
options such as:

```yaml
options:
  limit_domain: robot
  native_cfg_path: ./zenoh-client.json5
```

Relative package, extension, DDS `xml_profile_file` and Zenoh `native_cfg_path`
paths resolve from the recorder configuration directory. Missing schemas for
selected topics cause an explicit error. Network metadata never supplies paths
for loading executable code.

| Source | Subscription address | Bag type / serialization |
|--------|----------------------|--------------------------|
| DDS | Discovered native DDS topic/type, without another name conversion | `dds:<native type>` / `cdr` |
| ROS 2 | ROS graph topic and `pkg/msg/Type` | `ros2:pkg/msg/Type` / `ros2` |
| Zenoh | Actual wire key from metadata | Original logical type / actual envelope format |

Zenoh learns the serialization format from each received sample and validates
its schema before recording it. With `--serialization-format auto`, a package
providing only matching schemas is sufficient; format conversion also requires
TypeSupport serialization functions. Missing schemas stop recording with an
error when the corresponding sample arrives. Topics without received samples
do not create channels with an unknown format. DDS and ROS 2 adapt
reliability/durability to all observed publishers, ignoring subscriber QoS when
computing offered policies. All three backends share dynamic capture planning.

`--domain` selects the DDS or ROS 2 domain. It defaults to configured `domain_id`,
then `ROS_DOMAIN_ID` for ROS 2, otherwise 0. Explicit CLI and configuration values
must agree. Zenoh `--zenoh-domain` must likewise agree with `limit_domain`.
Zenoh discovery and capture own separate sessions using the same native config
by default; use a client config or automatically assigned listen ports. If the
capture config specifies a fixed listen port or session ID, pass a separate
observer config with `--zenoh-config <path>`.

Recording collects an initial two-second snapshot and keeps the observer alive
for late publishers. Zenoh removes instances after two seconds without a
metadata reply. `--no-discovery` disables subsequent graph updates; existing
network subscriptions can still receive new publishers on their topic. Each
invocation selects one source. `-a` selects all topics in that source, and `-d`
remains the split duration. `--use-sim-time` supports local, ROS 2 and Zenoh
sources exposing logical `/clock` topics.

The DDS observer uses default discovery settings for the selected domain;
custom transport settings apply to the capture driver. Generic keyed DDS types
remain unsupported. Bags retain native DDS names; playback mapping back to
these names is outside this recording change. Raw DDS mode does not map `/clock`.

---

### Complete Example

```yaml
ibmw:
  extensions:
    items:
      - name: dds_ext
        path: ./lib/libibmw_dds_transport.so
        options:
          domain_id: 0
          participant_name: my_node

  scheduling:
    - name: work_pool
      type: thread_pool
      options:
        thread_num: 4

    - name: timer_exec
      type: timer
      options:
        dt_us: 1000

  logging:
    core_level: Info
    default_module_level: Info
    backends:
      - type: console
        options:
          color: true
      - type: rotate_file
        options:
          path: /var/log/ibmw
          filename: app.log
          max_file_size_m: 50
          max_file_num: 10

  messaging:
    drivers:
      - type: local
        options:
          subscriber_executor: work_pool
      - type: dds
        options:
          subscribers:
            - topic: /sensor_data
              qos:
                reliability: best_effort
                depth: 1
              executor: work_pool
    publishers:
      - topic: ".*"
        enable_drivers: [local, dds]
    subscribers:
      - topic: ".*"
        enable_drivers: [local, dds]

  service:
    drivers:
      - type: local
        options:
          timeout_executor: timer_exec
    clients:
      - func_name: ".*"
        enable_drivers: [local]
    servers:
      - func_name: ".*"
        enable_drivers: [local]

  modules:
    packages:
      - path: ./lib/libmy_module_pkg.so
    modules:
      - name: SensorModule
        log_level: Debug
      - name: ProcessorModule
        log_level: Info

SensorModule:
  device: /dev/ttyUSB0
  publish_rate_hz: 100

ProcessorModule:
  algorithm: kalman_filter
  output_topic: /processed_data
```

---

## Troubleshooting

### DDS demos fail with shared library errors

Ensure the ROS2 libraries are on the library path:
```bash
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:<ibmw-build-dir>/lib:<ibmw-build-dir>:<ibmw-build-dir>/bin:$LD_LIBRARY_PATH"
```

### Build fails with "FastCDR not found"

Install FastCDR or ensure `_deps/fastcdr-install/` exists. With ROS2 Humble:
```bash
sudo apt install ros-humble-fastrtps
```

### DDS demos hang on shutdown

Some DDS demos take 30-60 seconds to shut down due to DDS discovery cleanup.
Use `timeout -s KILL <seconds>` to force-terminate:
```bash
timeout -s KILL 10 ./dds_native_app ./dds_native_cfg.yaml
```

### Module .so not found

Ensure the path in your YAML config is correct relative to the working directory.
Use absolute paths or `${ENV_VAR}` substitution for portability.

### Permission denied for real-time scheduling

Real-time executor demos (`SCHED_FIFO`, `SCHED_RR`) require elevated privileges:
```bash
sudo ./bin/ibmw run ./cfg/examples_cpp_executor_real_time_cfg.yaml
```

Or set capabilities:
```bash
sudo setcap cap_sys_nice+ep ./bin/ibmw
```

### ibmw-gen project cmake fails with "Could not find a configuration file for package ibmw"

After `cmake --install . --prefix .`, CMake generates `ibmw-config.cmake` and
`ibmw-config-version.cmake` in the build directory. These stale files can
interfere with ibmw-gen projects that use `-DCMAKE_PREFIX_PATH=<build-dir>`,
causing CMake to find an incomplete config instead of the correct targets.

**Fix:** Delete the stale files after install:
```bash
cd build
rm -f ibmw-config.cmake ibmw-config-version.cmake
```

This is already included in the standard build instructions above.
