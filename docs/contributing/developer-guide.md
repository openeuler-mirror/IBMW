# IBMW Developer Guide

This guide covers the IBMW build system, code structure, and how to develop
modules and extensions.

## Table of Contents

1. [Project Structure](#project-structure)
2. [Build System](#build-system)
   - [Cross-compilation configure support](#cross-compilation-configure-support)
3. [Writing a Module](#writing-a-module)
4. [Writing an Extension](#writing-an-extension)
5. [Type System](#type-system)
6. [Transport Development](#transport-development)
7. [RPC Development](#rpc-development)
8. [Coroutine Support](#coroutine-support)
9. [Logging](#logging)
10. [Red Lines and Multi-Driver Modules](#red-lines-and-multi-driver-modules)
11. [Testing](#testing)

---

## Project Structure

```
ibmw/
  CMakeLists.txt          # Top-level build configuration
  VERSION                 # Project version
  cmake/                  # CMake modules
  |  GetFastDDS.cmake     #   FastDDS dependency
  |  GetAsio.cmake        #   Asio dependency
  |  DdsIdlGenCode.cmake  #   IDL code generation
  |  ...
  src/
    base/                 # Base utilities
    |  log_util.h         #   SimpleLogger, SimpleAsyncLogger
    |  string_util.h      #   FormatTree, string helpers
    |  ...
    core/                 # Core runtime engine
    |  engine/            #   Engine subsystem implementations
    |  |  engine.h/cc     #     Central orchestrator (lifecycle, hooks)
    |  |  executor/       #     ExecutorBus + 5 executor types
    |  |  extension/      #     ExtensionBus (dynlib loading)
    |  |  logger/         #     LoggingSubsystem + formatter + 2 drivers
    |  |  module/         #     ModuleBus, ConfigSubsystem, ParameterStore, MemoryBus
    |  |  service/        #     ServiceDispatcher (RPC routing)
    |  |  transport/      #     TransportBus (pub/sub routing)
    |  main/              #   ibmw binary entry point
    |  |  main.cc         #     CLI parsing, Engine lifecycle
    |  extension_api/     #   Public API for extension development
    api/                  # Public API for module development
    |  c/                 #   C ABI layer
    |  cpp/               #   C++ SDK
    |  pkg/               #   Package entry point macros
    |  type_support/      #   Type support package ABI
    extensions/           # Extension implementations
    |  transport/         #   Transport backends
    |  |  dds_ext/        #     FastDDS (transport + RPC)
    |  |  net_ext/        #     HTTP/TCP/UDP
    |  |  mqtt_ext/       #     MQTT
     |  |  ros2_ext/       #     ROS2 **(deprecated)**
    |  |  iceoryx_ext/    #     Iceoryx (shared memory)
    |  |  zenoh_ext/      #     Zenoh
    |  rpc/
    |  |  grpc_ext/       #     gRPC
    |  observability/     #     Log Control, Topic Logger, OpenTelemetry
    |  management/        #     Parameter, Time Manipulator, Service Introspection,
    |  |                  #     Record/Playback
    |  utility/           #     Echo, Proxy
    schema/               # Type definitions
    |  dds_idl/           #   DDS IDL + generated code
    |  ros2/              #   ROS2 msg/srv definitions
     |  extensions/        #   Extension-specific types (dds_ext_proto, ros2_ext_proto **(deprecated)**)
    samples/cpp/          # example applications and verification samples
    tools/                # CLI tools and code generators
```

---

## Build System

### CMake Architecture

The top-level `CMakeLists.txt` defines all build options and orchestrates
dependency fetching via modules in `cmake/`.

Key CMake modules:

| Module | Purpose |
|--------|---------|
| `GetAsio.cmake` | Fetch Asio (header-only) |
| `GetFastDDS.cmake` | Fetch/find FastDDS + FastCDR |
| `GetYamlCpp.cmake` | Fetch yaml-cpp |
| `GetTbb.cmake` | Fetch Intel TBB |
| `GetFmt.cmake` | Fetch fmt library |
| `GetNlohmannJson.cmake` | Fetch nlohmann/json |
| `GetBoost.cmake` | Fetch Boost (net/grpc extensions) |
| `DdsIdlGenCode.cmake` | Generate FastCDR code from DDS IDL files |
| `GetGTest.cmake` | Fetch Google Test |

### Adding a New Dependency

1. Create `cmake/GetMyDep.cmake`:
```cmake
include(FetchContent)
FetchContent_Declare(
  mydep
  GIT_REPOSITORY https://github.com/example/mydep.git
  GIT_TAG v1.0.0
)
FetchContent_MakeAvailable(mydep)
```

2. Include it from the relevant `CMakeLists.txt`:
```cmake
include(${CMAKE_SOURCE_DIR}/cmake/GetMyDep.cmake)
```

### Build Configuration

```bash
# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Release with all extensions (recommended: use CMake presets)
cmake --preset full
cmake --build build -j$(nproc)

# Or manually:
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_NET_EXTENSION=ON \
  -DIBMW_BUILD_GRPC_EXTENSION=ON \
  -DIBMW_BUILD_MQTT_EXTENSION=ON \
  -DIBMW_BUILD_ECHO_EXTENSION=ON \
  -DIBMW_BUILD_PROXY_EXTENSION=ON \
  -DIBMW_BUILD_PARAMETER_EXTENSION=ON \
  -DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON \
  -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON \
  -DIBMW_BUILD_TIME_MANIPULATOR_EXTENSION=ON \
  -DIBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION=ON \
  -DIBMW_BUILD_TOPIC_LOGGER_EXTENSION=ON
```

### Clearing Build Cache

If directory structures change, clear the CMake cache before rebuilding:

```bash
cd build && rm -rf CMakeCache.txt CMakeFiles
```

### Cross-compilation configure support

IBMW has configure-level cross-compilation support for aarch64 Linux through CMake presets, `cmake/IbmwCrossCompile.cmake`, and `cmake/toolchains/aarch64-linux-gnu.cmake`. The current model keeps host code generation separate from target package lookup and requires a target ROS 2 prefix for FastDDS, FastCDR and ROS 2 interface packages.

The direct target package list is intentionally small: `fastcdr`, `fastdds` / `fastrtps`, `std_msgs`, `geometry_msgs`, optional `sensor_msgs`, `ament_cmake`, `rosidl_default_generators`, and `builtin_interfaces`. Transitive packages are intentionally not listed because they should come from those packages' exported CMake metadata. `rclcpp` cross support is deferred and is not claimed by the current configure path.

Use target-prefix validation, a clean configure, target artifact inspection, and
an external install consumer for cross-build changes. Board deployment and
runtime remain separate validation steps.

See the [Cross-compilation guide](../guides/cross-compilation.md) and
[Support Matrix](../reference/support-matrix.md).

---

## Writing a Module

> **Tip:** You can skip writing boilerplate by hand. Run
> `ibmw-gen my_project.ibmw.yaml -o my_project` to generate module
> scaffolds, CMake rules, and YAML configs from a single definition file.
> See the [ibmw-gen reference](../../src/tools/ibmw_gen/README.md).

### Module Lifecycle

Every IBMW module is a C++ class that inherits from `ibmw::Module` and
implements four public lifecycle methods:

```cpp
#include "ibmw_sdk.h"  // or "cpp/core.h" + "cpp/module_base.h"

class MyModule : public ibmw::Module {
 public:
  // Return module metadata
  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{
        .module_name = "MyModule",
        .major_version = 1, .minor_version = 0,
        .patch_version = 0, .build_version = 0,
        .author = "My Team",
        .description = "Example module",
    };
  }

  // Called after loading -- register types, get handles
  bool Setup(ibmw::Runtime runtime) override {
    core_ = runtime;
    // Get subsystem handles
    auto executor_bus = core_.GetExecutor();
    auto transport = core_.GetTransport();
    auto logger = core_.GetLogger();
    auto params = core_.GetParams();
    auto configurator = core_.GetConfigurator();
    auto service = core_.GetService();
    return true;
  }

  // Called after all modules loaded -- start work
  bool Run() override {
    MW_INFO("Module started");
    return true;
  }

  // Called during shutdown -- clean up
  void Stop() override {
    MW_INFO("Module stopping");
  }

 private:
  ibmw::Runtime core_;
};
```

### Module Packaging (Pkg Mode)

To make a module loadable as a shared library by the `ibmw` runtime, create
a package entry point:

```cpp
// pkg_main.cc
#include "ibmw_pkg_abi.h"
#include "my_module.h"

static const auto kModuleArray = std::array{
    std::make_tuple("MyModule",
                    []() -> ibmw::Module* { return new MyModule(); }),
};

IBMW_PKG_MAIN(kModuleArray)
```

Build as a shared library:
```cmake
add_library(my_module_pkg SHARED
  my_module.cc
  pkg_main.cc
)
target_link_libraries(my_module_pkg PRIVATE ibmw_interface)
```

### Module as Standalone App (App Mode)

```cpp
// main.cc
#include "ibmw_sdk.h"
#include "my_module.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml>\n";
    return 1;
  }

  // Create engine and module
  ibmw::runtime::core::Engine engine;
  engine.Initialize(argv[1]);

  // Create and register module
  auto& module_bus = engine.GetModuleBus();
  module_bus.CreateModule<MyModule>();

  // Run (blocks until Ctrl+C)
  engine.AsyncStart();
  engine.WaitForShutdownSignal();
  engine.Shutdown();
  return 0;
}
```

### Accessing Per-Module Configuration

Modules can read their own YAML config section:

```yaml
# In the YAML config file
MyModule:
  sensor_rate: 100
  topic_name: /sensor_data
```

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  auto config_path = core_.GetConfigurator().GetConfigFilePath();
  // Parse config_path with yaml-cpp or another YAML library
  return true;
}
```

### Transport: Publishing Messages

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  publisher_ = core_.GetTransport().GetSender("my_topic");
  ibmw::transport::RegisterSendType<MyMessageType>(publisher_);
  return true;
}

bool Run() override {
  auto executor = core_.GetExecutor().GetExecutor("work_pool");
  executor.Dispatch([this]() {
    MyMessageType msg;
    msg.data = "Hello";
    ibmw::transport::Send(publisher_, msg);
  });
  return true;
}
```

### Transport: Subscribing to Messages

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  auto subscriber = core_.GetTransport().GetReceiver("my_topic");
  ibmw::transport::OnMessage<MyMessageType>(subscriber,
    [](const MyMessageType& msg) {
      MW_INFO("Received: {}", msg.data);
    });
  return true;
}
```

### RPC: Registering a Server

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  auto service = core_.GetService();

  // Register RPC server function
  RegisterMyServiceServerFunc(service);  // Generated from IDL

  // Set handler
  MyServiceSyncAccess access(service, "/my_service/Query");
  access.RegisterHandler([](const MyRequest& req, MyResponse& rsp) {
    rsp.result = process(req.input);
    return ibmw::rpc::Status();  // OK
  });

  return true;
}
```

### RPC: Making a Client Call

```cpp
// Synchronous
MyServiceSyncAccess access(service, "/my_service/Query");
MyRequest req; req.input = 42;
MyResponse rsp;
auto ctx = ibmw::rpc::Context(IBMW_RPC_CLIENT_CONTEXT);
ctx.SetTimeoutNs(3'000'000'000);  // 3 seconds
auto status = access.Invoke(ctx.GetContextRef(), req, rsp);
if (status.OK()) {
  MW_INFO("Result: {}", rsp.result);
}

// Coroutine
co::Task<void> MyCoroutine() {
  MyServiceCoAccess access(service, "/my_service/Query", executor);
  MyRequest req; req.input = 42;
  MyResponse rsp;
  auto ctx = ibmw::rpc::Context(IBMW_RPC_CLIENT_CONTEXT);
  auto status = co_await access.Invoke(ctx.GetContextRef(), req, rsp);
}
```

### Using the High-Level Context API

The Context API provides a builder-pattern interface that manages resource
lifetimes automatically:

```cpp
#include "cpp/context/context.h"

class MyModule : public ibmw::Module {
  ibmw::context::Context ctx_;
  ibmw::context::res::Publisher<MyMsg> pub_;
  ibmw::context::res::Subscriber<MyMsg> sub_;

  bool Setup(ibmw::Runtime runtime) override {
    ctx_ = ibmw::context::Context(runtime, "work_pool");

    // Create publisher
    ctx_.Pub<MyMsg>("my_topic").Use(pub_).Build();

    // Create subscriber with callback
    ctx_.Sub<MyMsg>("my_topic")
        .Use(sub_)
        .SetCallback([](const MyMsg& msg) {
          // Handle message
        })
        .Build();

    return true;
  }

  bool Run() override {
    MyMsg msg;
    msg.data = "Hello";
    pub_.Publish(msg);
    return true;
  }

  void Stop() override {
    ctx_.Shutdown();
  }
};
```

---

## Writing an Extension

Extensions are shared libraries that implement `EngineExtensionBase`. They
are loaded at runtime and can register transport drivers, RPC drivers,
log backends, and more.

### Extension Structure

```cpp
// my_extension.h
#include "ibmw_extension_api.h"

class MyExtension : public ibmw::EngineExtensionBase {
 public:
  std::string_view Name() const noexcept override {
    return "my_extension";
  }

  bool Initialize(ibmw::runtime::core::Engine* core_ptr) noexcept override {
    // Access engine subsystems
    // Register transport/RPC drivers
    // Read extension options from YAML
    return true;
  }

  void Shutdown() noexcept override {
    // Clean up resources
  }

  std::list<std::pair<std::string, std::string>>
  GenInitializationReport() const override {
    return {{"key", "value"}};  // Shown in startup log
  }
};
```

### Extension Entry Points

```cpp
// my_extension_main.cc
#include "my_extension.h"

IBMW_CORE_EXTENSION_EXPORT
ibmw::EngineExtensionBase* IbmwDynlibCreateCoreExtensionHandle() {
  return new MyExtension();
}

IBMW_CORE_EXTENSION_EXPORT
void IbmwDynlibDestroyCoreExtensionHandle(
    const ibmw::EngineExtensionBase* ptr) {
  delete ptr;
}
```

### Extension Logging

Use the extension logger macros for per-extension logging:

```cpp
// global.h
#include "extension_logger.h"
IBMW_DECLARE_EXTENSION_LOGGER(my_ext)

// global.cc
IBMW_DEFINE_EXTENSION_LOGGER(my_ext)
```

### Extension CMake

```cmake
add_library(my_extension SHARED
  my_extension.cc
  my_extension_main.cc
)
target_link_libraries(my_extension PRIVATE ibmw_extension_api)
```

### Extension YAML Config

Extensions receive their options from the YAML config:

```yaml
ibmw:
  extensions:
    items:
      - name: my_extension
        path: ./lib/libmy_extension.so
        options:
          my_option: value
```

The `options` node is passed to the extension's `Initialize()` method via
the engine's configuration subsystem.

---

## Type System

IBMW uses DDS IDL as the primary type definition language with FastCDR
for serialization. There is no protobuf dependency.

### Defining Types with DDS IDL

Create an IDL file:

```idl
// my_types.idl
module protocols {
  struct SensorData {
    uint64 timestamp;
    float temperature;
    float humidity;
    string sensor_id;
  };
};
```

### Code Generation

The `DdsIdlGenCode.cmake` module generates FastCDR serialization code:

```cmake
include(DdsIdlGenCode)
ibmw_dds_idl_gen(
  TARGET my_types
  IDL_FILES my_types.idl
)
```

This generates:
- `my_types.h` -- Type definitions
- `my_types_cdr.h/.cc` -- FastCDR serialize/deserialize functions
- `my_types_dds_types.h` -- DDS TopicDataType adapter

### Hand-Written Types (No Code Generation)

You can also define types manually:

```cpp
#include "ibmw/extensions/transport/dds_types/util/dds_type_support.h"

struct HelloMsg {
  uint64_t id;
  std::string message;
  int64_t timestamp;

  IBMW_DDS_TYPE_NAME("HelloMsg")

  void serialize(eprosima::fastcdr::Cdr& cdr) const {
    cdr << id << message << timestamp;
  }

  void deserialize(eprosima::fastcdr::Cdr& cdr) {
    cdr >> id >> message >> timestamp;
  }
};

// Register FastCDR v1 free functions
template<>
size_t calculate_serialized_size(
    eprosima::fastcdr::CdrSizeCalculator& calc,
    const HelloMsg& msg, size_t& current_alignment);

template<>
void serialize(eprosima::fastcdr::Cdr& cdr, const HelloMsg& msg);

template<>
void deserialize(eprosima::fastcdr::Cdr& cdr, HelloMsg& msg);
```

### JSON Types

For management/service types, IBMW supports JSON serialization via nlohmann/json:

```cpp
#include "cpp/util/json_type_support.h"

struct MyConfig {
  static constexpr char kTypeName[] = "my_app::MyConfig";
  std::string name;
  int value;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(MyConfig, name, value)
};

// Get type support
const auto* ts = ibmw::json::GetJsonMessageTypeSupport<MyConfig>();
```

### Type Support Packages

For runtime type discovery (used by Echo, Proxy, Record/Playback extensions),
create a type support package:

```cpp
#include "ibmw_type_support_abi.h"

static const ibmw_type_support_base_t* kTypeSupportArray[] = {
    GetSensorDataTypeSupport(),
    GetImageTypeSupport(),
};

IBMW_TYPE_SUPPORT_PKG_EXPORT
size_t IbmwDynlibGetTypeSupportArrayLength() {
  return sizeof(kTypeSupportArray) / sizeof(kTypeSupportArray[0]);
}

IBMW_TYPE_SUPPORT_PKG_EXPORT
const ibmw_type_support_base_t** IbmwDynlibGetTypeSupportArray() {
  return kTypeSupportArray;
}
```

---

## Transport Development

### Architecture

The transport subsystem uses a driver-based architecture:

```
TransportBus
  |-- TransportDriver (local)
  |-- TransportDriver (dds)
  |-- TransportDriver (mqtt)
  |-- ...
```

Each driver implements:
- Publisher registration and message sending
- Subscriber registration and message delivery
- Topic-to-driver routing (configured in YAML)

### Routing

Transport routing is regex-based. Publishers and subscribers are matched to
drivers via `enable_drivers`:

```yaml
messaging:
  publishers:
    - topic: "/sensor/.*"
      enable_drivers: [dds]
    - topic: "/internal/.*"
      enable_drivers: [local]
  subscribers:
    - topic: ".*"
      enable_drivers: [local, dds]
```

### Filters

Transport filters intercept messages before/after sending or receiving.
Configured via `enable_filters` in the routing rules.

---

## RPC Development

### Architecture

The RPC subsystem mirrors the transport architecture:

```
ServiceDispatcher
  |-- RpcDriver (local)       -- In-process, no network
  |-- RpcDriver (dds)         -- Pure-DDS, ROS2 wire-compatible
  |-- RpcDriver (grpc)        -- gRPC remote
  |-- RpcDriver (http)        -- HTTP remote
  |-- ...
```

The `dds` driver is registered by the DDS extension. It uses **native `.srv`
mode**: business request/response types are directly serialized/deserialized by
FastDDS via the TypeSupport obtained from gencode
(`info.req_type_support_ref.CustomTypeSupportPtr()`). There are no envelope
wrapper types. For a matching, validated ROS2 release, FastDDS/Fast-CDR profile,
generated type, QoS, and request/reply correlation contract, the CDR payload is
designed to match ROS2 services. This is not a universal cross-version/vendor
compatibility guarantee; each service product still requires typed bidirectional
acceptance evidence.

The driver implements the ROS2 Service wire protocol (WriteParams/SampleInfo
correlation, `rq/`/`rr/` topic naming, `dds_::` type name infixes) using only
the FastDDS public API -- no rcl/rclcpp dependency. When `ros2_compatible: true`,
IBMW services are discoverable and callable by standard ROS2 nodes
(`ros2 service call`, `ros2 service list`).

Key implementation details:
- TypeSupport is injected by gencode, not hardcoded in the driver
- DDS type names follow the ROS2 convention: `<pkg>::srv::dds_::<Name>_Request_`
- `dds_rpc_types.h` (envelope wrappers) has been removed
- The `srv:` field in ibmw-gen auto-derives `req_dds_type_name`,
  `rsp_dds_type_name`, and `ros2_service_name` from the `.srv` package path

### Four Calling Patterns

| Pattern | Access Class | Blocking | Return |
|---------|-------------|----------|--------|
| Synchronous | `SyncAccessBase<Req, Rsp>` | Yes | `rpc::Status` |
| Async callback | `AsyncAccessBase<Req, Rsp>` | No | via callback |
| Future | `FutureAccessBase<Req, Rsp>` | No | `std::future<rpc::Status>` |
| Coroutine | `CoAccessBase<Req, Rsp>` | No | `co::Task<rpc::Status>` |

### RPC Filters

Coroutine-based RPC filters intercept calls for cross-cutting concerns:

```cpp
class TimingFilter : public ibmw::CoRpcFilter {
  co::Task<rpc::Status> Handle(CoRpcHandle& handle,
                                CoFilterBus& next) override {
    auto start = std::chrono::steady_clock::now();
    auto status = co_await next.Next(handle);
    auto elapsed = std::chrono::steady_clock::now() - start;
    MW_INFO("RPC took {}us",
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    co_return status;
  }
};
```

Register filters on server operations:

```cpp
ctx_.Srv<Req, Rsp>("/my_service/Query")
    .Use(srv_)
    .SetCallback(handler)
    .AddFilter(std::make_shared<TimingFilter>())
    .Build();
```

---

## Coroutine Support

IBMW provides C++20 coroutine utilities via the `ibmw::co` namespace:

### Scheduling

```cpp
// Schedule work on an executor
co_await co::Schedule(executor);

// Schedule after a delay
co_await co::ScheduleAfter(executor, std::chrono::seconds(1));

// Schedule at a specific time
co_await co::ScheduleAt(executor, time_point);
```

### Task

```cpp
co::Task<int> ComputeAsync() {
  co_await co::Schedule(executor);
  co_return 42;
}
```

### AsyncScope (Structured Concurrency)

```cpp
co::AsyncScope scope;

// Spawn multiple concurrent tasks
scope.spawn(co::On(scheduler, Task1()));
scope.spawn(co::On(scheduler, Task2()));

// Wait for all tasks
co_await scope.join();
```

### SyncWait (Blocking)

```cpp
auto result = co::SyncWait(ComputeAsync());
```

---

## Logging

### Log Macros

```cpp
MW_TRACE("Trace message: {}", value);
MW_DEBUG("Debug message: {}", value);
MW_INFO("Info message: {}", value);
MW_WARN("Warning: {}", value);
MW_ERROR("Error: {}", value);
MW_FATAL("Fatal: {}", value);
```

### Log Levels

| Level | Value | Description |
|-------|-------|-------------|
| Trace | 0 | Detailed tracing |
| Debug | 1 | Debug information |
| Info | 2 | General information (default) |
| Warn | 3 | Warnings |
| Error | 4 | Errors |
| Fatal | 5 | Fatal errors |
| Off | 6 | Logging disabled |

### Log Format Pattern

The default pattern is: `%l %c.%f [%n] %v (%G:%R)`

| Specifier | Description |
|-----------|-------------|
| `%l` | Log level |
| `%c` | Date and time |
| `%f` | Microseconds |
| `%n` | Module name |
| `%v` | Log message |
| `%G` | Short filename |
| `%R` | Line number |
| `%g` | Full file path |
| `%F` | Function name |
| `%t` | Thread ID |

---

## Multi-Driver Modules

IBMW enforces a set of code-quality red-line invariants around transport
decoupling. They are checked by `scripts/check_red_lines.sh` and documented
for contributors in `skills/transport_decoupling.md`.

### `IBMW_USE_DRIVER` vs `IBMW_USE_DRIVERS`

The supported way for user-region modules to obtain a `*TransportDriver*`
pointer is `IBMW_USE_DRIVER` (single driver) or `IBMW_USE_DRIVERS`
(multiple drivers). Both expand to a `Module::SetDriverSlot(slot_id,
driver_ptr)` override that the Engine invokes when broadcasting
driver-slot ids during `Start()`.

**Single-driver module:**
```cpp
class MyModule : public ibmw::Module {
 private:
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  IBMW_USE_DRIVER(ibmw::extensions::dds_extension::DdsTransportDriver, dds_drv_)
};
```

**Multi-driver module:**
```cpp
class MyModule : public ibmw::Module {
 private:
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* iox_drv_ = nullptr;
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver, dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxTransportDriver, iox_drv_))
};
```

`IBMW_DRIVER_ENTRY` matches the broadcast `slot_id` against
`DriverT::kStableDriverId` (a stable identifier injected by
`IBMW_DECLARE_DRIVER_SLOT` in the driver header) and, on match, performs
a `static_cast<DriverT*>` assignment. Any typo in a driver type name
fails at compile time.

> Full end-to-end sample: `src/samples/cpp/multi_driver_demo/`. Macro
> definitions: `src/api/cpp/module_base.h`. Slot-id entry point:
> `src/api/c/driver_slot.h`.

---

## Testing

### Running Tests

```bash
cmake -B build -DIBMW_BUILD_TESTS=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Testing Demos

Most demos do not self-terminate. Use `timeout` for automated testing:

```bash
# Normal demos (respond to SIGINT)
timeout -s INT 5 ./bin/ibmw run ./cfg/my_config.yaml

# DDS demos (may need SIGKILL due to slow shutdown)
timeout -s KILL 10 ./dds_native_app ./cfg/dds_native_cfg.yaml
```

Exit codes: 0/124/130 = success (124=timeout, 130=SIGINT).

### Writing Unit Tests

```cpp
#include <gtest/gtest.h>
#include "my_module.h"

TEST(MyModuleTest, BasicTest) {
  // Test module logic without the IBMW runtime
  MyModule module;
  // ...
}
```
