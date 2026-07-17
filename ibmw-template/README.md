# IBMW External Project Template

Starter project for building **Intelligence Boom Robot Middleware (IBMW)**
modules and applications outside the IBMW source tree.

This document is written for developers who are **new to IBMW**. It walks
through every step end-to-end: installing prerequisites, understanding the
three message type systems, running the 8 included demos plus the extension-loading example, and -- most
importantly -- **creating your own custom message types from scratch**.

---

## Table of Contents

1.  [Overview of Demos](#overview-of-demos)
2.  [Prerequisites and Tool Installation](#prerequisites-and-tool-installation)
3.  [Quick Start](#quick-start)
4.  [Quick Start with ibmw-gen](#quick-start-with-ibmw-gen)
5.  [Project Structure](#project-structure)
6.  [Two Operating Modes](#two-operating-modes)
7.  [Writing Your First Module](#writing-your-first-module)
8.  [Three Message Type Systems](#three-message-type-systems)
9.  [Deep Dive: DDS IDL Messages](#deep-dive-dds-idl-messages)
10. [Deep Dive: ROS2 Messages](#deep-dive-ros2-messages)
11. [Deep Dive: JSON Messages](#deep-dive-json-messages)
12. [Demo 8: DDS Pub/Sub With Your Own IDL](#demo-8-dds-pubsub-with-your-own-idl)
13. [YAML Configuration Reference](#yaml-configuration-reference)
13. [Plain vs Non-Plain Types](#plain-vs-non-plain-types)
14. [Context API and LetMe](#context-api-and-letme)
15. [Loading Extensions](#loading-extensions)
16. [Switching to DDS or gRPC Drivers](#switching-to-dds-or-grpc-drivers)
17. [CMake Targets Reference](#cmake-targets-reference)
18. [CMake Presets](#cmake-presets)
19. [Executor Types Reference](#executor-types-reference)
20. [License](#license)

---

## Overview of Demos

| # | Demo | Message System | What It Shows |
|---|------|----------------|---------------|
| 1 | Minimal module | -- | Module lifecycle (Setup / Run / Stop), pkg + app mode |
| 2 | DDS Pub/Sub (schema-shipped) | DDS IDL | Reusing IBMW's pre-built `ExampleEventMsg` -- **no `.idl` and no `cdr_impl.cc` needed** |
| 3 | DDS RPC (schema-shipped) | DDS IDL | Reusing IBMW's pre-built `ExampleService` -- **no `.idl` and no `cdr_impl.cc` needed** |
| 4 | ROS2 Pub/Sub | ROS2 msg | Publishing and subscribing with ROS2 message types **(deprecated -- requires rcl)** |
| 5 | ROS2 RPC | ROS2 srv | ROS2 service-type RPC with generated coroutine server and client **(deprecated -- use Demo 3 DDS RPC instead)** |
| 6 | JSON Pub/Sub | JSON | Custom C++ structs with JSON serialization, no IDL needed |
| 7 | JSON RPC | JSON | JSON-based RPC using ServiceBase + ServiceRef::Call |
| 8 | DDS Pub/Sub (bring-your-own-IDL) | DDS IDL | **Defining your own `.idl` end-to-end**: fastddsgen codegen + hand-written `cdr_impl.cc` + type bridge |
| - | Extension loading | -- | Loading an IBMW extension (.so plugin) from YAML config |

> **Two DDS workflows.** Demos 2 / 3 show the *schema-shipped* path: when the
> message type already lives inside an installed IBMW schema package
> (`ibmw::schema::example_idl_gencode`, `ibmw::schema::example_rpc_gencode`),
> the CDR serialization object code is shipped inside the package itself, so
> downstream projects don't write a single line of `cdr_impl.cc`. Demo 8 shows
> the full *bring-your-own-IDL* path that you need whenever you define a new
> `.idl` file in your own project.

All demos default to the **local** driver (in-process communication). Maintained
configuration examples are in [`cfg/`](cfg/).

---

## Prerequisites and Tool Installation

### 1. C++ Compiler

GCC 11+ or Clang 14+ with C++20 support.

```bash
g++ --version   # Must be >= 11
```

### 2. CMake 3.24+

```bash
cmake --version
```

### 3. ROS 2 Humble

Required for Demos 4 and 5, and whenever IBMW was built with
`-DIBMW_BUILD_WITH_ROS2=ON`. Follow the
[official ROS 2 Humble installation guide](https://docs.ros.org/en/humble/Installation.html),
then source it before every build/run session:

```bash
source /opt/ros/humble/setup.bash
```

### 4. Fast DDS Code Generator (fastddsgen) -- only for custom DDS IDL types

> You do **NOT** need fastddsgen for Demos 1-7 or the extension-loading example.
> They use IBMW's pre-built schema types. Demo 8 is the bring-your-own-IDL path
> and uses fastddsgen because it defines a new `.idl` message type.

fastddsgen is a Java-based tool from eProsima that reads `.idl` files and
generates C++ type definitions + CDR serialization code.

**Install JDK 11+** (fastddsgen requires Java):

```bash
sudo apt install default-jdk
java -version   # Must be >= 11
```

**Install fastddsgen v4.x**:

```bash
git clone https://github.com/eProsima/Fast-DDS-Gen.git
cd Fast-DDS-Gen
git checkout v4.0.2
./gradlew assemble
sudo cp scripts/fastddsgen /usr/local/bin/
sudo mkdir -p /usr/local/share/fastddsgen/java
sudo cp share/fastddsgen/java/fastddsgen.jar /usr/local/share/fastddsgen/java/
```

Verify:

```bash
fastddsgen --version
```

### 5. Build and Install IBMW

```bash
source /opt/ros/humble/setup.bash

export IBMW_SOURCE_DIR="<ibmw-source-dir>"
cd "$IBMW_SOURCE_DIR"
mkdir -p build && cd build
cmake .. \
  -DIBMW_BUILD_TESTS=OFF \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ECHO_EXTENSION=ON \
  -DIBMW_BUILD_PROXY_EXTENSION=ON \
  -DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON \
  -DIBMW_BUILD_PARAMETER_EXTENSION=ON \
  -DIBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION=ON \
  -DIBMW_BUILD_TIME_MANIPULATOR_EXTENSION=ON \
  -DIBMW_BUILD_TOPIC_LOGGER_EXTENSION=ON \
  -DIBMW_BUILD_NET_EXTENSION=ON \
  -DIBMW_BUILD_GRPC_EXTENSION=ON \
  -DIBMW_BUILD_MQTT_EXTENSION=ON \
  -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON

make -j$(nproc)

# Install into the build directory (creates lib/, bin/, include/, share/)
cmake --install . --prefix .
```

After install, `build/lib/cmake/ibmw/` contains CMake config files and
`build/bin/` contains helper tools (`ibmw`, `dds_idl_gen_rpc.py`,
`ros2_gen_ibmw_cpp_rpc.py`).

---

## Quick Start

```bash
source /opt/ros/humble/setup.bash
export IBMW_BUILD_DIR="<ibmw-build-dir>"

cd ibmw-template
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# App mode -- standalone executable
./pubsub_app cfg/pubsub_app.yaml

# Pkg mode -- dynamic plugin loaded by ibmw runner
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/pubsub_pkg.yaml
```

All demos run indefinitely. Press Ctrl-C to stop.

---

## Quick Start with ibmw-gen

Instead of hand-writing modules, CMake rules, and YAML configs, you can use
**ibmw-gen** to generate a complete project from a single `.ibmw.yaml`
definition file.

This template ships with [`ibmw_template.ibmw.yaml`](ibmw_template.ibmw.yaml),
which describes all 8 demos. The pre-generated output lives in
[`my_ibmw_project/`](my_ibmw_project/).

### Generate from scratch

```bash
# From the ibmw-template directory:
export IBMW_SOURCE_DIR="<ibmw-source-dir>"
python3 "$IBMW_SOURCE_DIR/src/tools/ibmw_gen/ibmw_gen.py" \
    ibmw_template.ibmw.yaml -o my_ibmw_project
```

This produces the full project under `my_ibmw_project/`:

```
my_ibmw_project/
  CMakeLists.txt          # Ready-to-build CMake project
  cfg/                    # YAML configs for every demo (app + pkg)
  src/                    # Module scaffolds with TODO markers
```

### Build and run

```bash
source /opt/ros/humble/setup.bash
export IBMW_BUILD_DIR="<ibmw-build-dir>"
mkdir -p my_ibmw_project/build && cd my_ibmw_project/build
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# App mode
./pubsub_app cfg/pubsub_app.yaml

# Pkg mode
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/pubsub_pkg.yaml
```

### Why use ibmw-gen?

| Manual (this template) | ibmw-gen |
|------------------------|----------|
| Write module `.h`/`.cc`, CMake targets, YAML configs, type bridges by hand | Declare everything in one `.ibmw.yaml`, generate the rest |
| Full control over every line | Scaffolds with TODO markers; fill in business logic |
| Good for learning IBMW internals | Good for quickly bootstrapping new projects |

Both approaches produce equivalent code. The hand-written demos in this
template and the generated `my_ibmw_project/` are functionally identical.

For the full ibmw-gen reference, see
[`src/tools/ibmw_gen/README.md`](../src/tools/ibmw_gen/README.md).

---

## Project Structure

```
ibmw-template/
  CMakeLists.txt              # Build rules for all 8 demos
  CMakePresets.json            # Convenient build presets
  README.md                   # This file
  ibmw_template.ibmw.yaml     # ibmw-gen project definition (all 8 demos)
  my_ibmw_project/             # Pre-generated output from ibmw-gen
  cfg/                        # YAML configs (one per demo per mode)
    my_module_pkg.yaml         #   Demo 1 pkg
    my_app.yaml                #   Demo 1 app
    pubsub_pkg.yaml            #   Demo 2 pkg
    pubsub_app.yaml            #   Demo 2 app
    rpc_pkg.yaml               #   Demo 3 pkg
    rpc_app.yaml               #   Demo 3 app
    ros2_pubsub_pkg.yaml       #   Demo 4 pkg
    ros2_pubsub_app.yaml       #   Demo 4 app
    ros2_rpc_pkg.yaml          #   Demo 5 pkg
    ros2_rpc_app.yaml          #   Demo 5 app
    json_pubsub_pkg.yaml       #   Demo 6 pkg
    json_pubsub_app.yaml       #   Demo 6 app
    json_rpc_pkg.yaml          #   Demo 7 pkg
    json_rpc_app.yaml          #   Demo 7 app
    my_msg_pubsub_pkg.yaml     #   Demo 8 pkg
    my_msg_pubsub_app.yaml     #   Demo 8 app
    extension_demo_pkg.yaml    #   Extension loading
  cfg/                         # Maintained application and package configurations
  src/
    my_module/                 # Demo 1: minimal module
    my_module_pkg/             #   pkg entry point
    my_app/                    #   app entry point
    example_event_dds_types.h  # DDS type bridge header (Demos 2-3, schema-shipped)
    publisher_module/          # Demo 2: DDS publisher
    subscriber_module/         # Demo 2: DDS subscriber
    pubsub_pkg/ pubsub_app/
    rpc_server_module/         # Demo 3: DDS RPC server
    rpc_client_module/         # Demo 3: DDS RPC client
    rpc_pkg/ rpc_app/
    my_msg/                    # Demo 8: bring-your-own-IDL
      MyMsg.idl                #   user-defined IDL file
      my_msg_cdr_impl.cc       #   CDR impl TU (the only place .ipp is compiled)
      my_msg_dds_types.h       #   type bridge header
    my_msg_publisher_module/   # Demo 8: publisher
    my_msg_subscriber_module/  # Demo 8: subscriber
    my_msg_pubsub_pkg/ my_msg_pubsub_app/
    ros2_publisher_module/     # Demo 4: ROS2 publisher
    ros2_subscriber_module/    # Demo 4: ROS2 subscriber
    ros2_pubsub_pkg/ ros2_pubsub_app/
    ros2_rpc_server_module/    # Demo 5: ROS2 RPC server
    ros2_rpc_client_module/    # Demo 5: ROS2 RPC client
    ros2_rpc_pkg/ ros2_rpc_app/
    json_custom_types.h        # JSON type definitions (Demos 6-7)
    json_publisher_module/     # Demo 6: JSON publisher
    json_subscriber_module/    # Demo 6: JSON subscriber
    json_pubsub_pkg/ json_pubsub_app/
    json_rpc_server_module/    # Demo 7: JSON RPC server
    json_rpc_client_module/    # Demo 7: JSON RPC client
    json_rpc_pkg/ json_rpc_app/
```

---

## Two Operating Modes

IBMW modules can run in two modes. Both produce identical runtime behavior.

### Pkg Mode (Dynamic Plugin)

Your module compiles to a `.so` shared library. The `ibmw` binary loads it
at runtime based on the YAML config.

**YAML** (`cfg/my_module_pkg.yaml`):
```yaml
ibmw:
  modules:
    packages:
      - path: ./libmy_module_pkg.so    # .so built by CMake
    modules:
      - name: MyModule
        log_level: INFO
```

**Entry point** (`pkg_main.cc`):
```cpp
#include "pkg/pkg_macro.h"
#include "my_module/my_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"MyModule", []() -> ibmw::Module* {
           return new my_project::MyModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
```

The string `"MyModule"` **must match** the `name:` in the YAML modules list.

**Run**:
```bash
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/my_module_pkg.yaml
```

### App Mode (Standalone Executable)

Your module compiles into a self-contained executable that embeds the IBMW
engine. No `.so` loading needed.

**Entry point** (`main.cc`):
```cpp
#include "cpp/core.h"
#include "engine/engine.h"

using namespace ibmw::runtime::core;

int32_t main(int32_t argc, char** argv) {
    Engine core;

    // Register modules before Initialize
    my_project::PublisherModule pub;
    my_project::SubscriberModule sub;
    core.GetModuleBus().RegisterModule(pub.NativeHandle());
    core.GetModuleBus().RegisterModule(sub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    core.Initialize(options);
    core.Start();       // Blocks until Shutdown()
    core.Shutdown();
    return 0;
}
```

**Run**:
```bash
./pubsub_app cfg/pubsub_app.yaml
```

### CMake for Both Modes

```cmake
# Shared module library
add_library(my_module STATIC src/my_module/my_module.cc)
target_link_libraries(my_module PUBLIC ibmw::api::cpp)
set_target_properties(my_module PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Pkg mode (.so plugin)
add_library(my_module_pkg SHARED src/my_module_pkg/pkg_main.cc)
target_link_libraries(my_module_pkg PRIVATE ibmw::api::pkg my_module)

# App mode (executable)
add_executable(my_app src/my_app/main.cc)
target_link_libraries(my_app PRIVATE ibmw::core::engine my_module)
```

---

## Writing Your First Module

Every IBMW module subclasses `ibmw::Module` and implements three lifecycle
methods:

```cpp
// my_module.h
#pragma once
#include "cpp/module_base.h"

namespace my_project {

class MyModule : public ibmw::Module {
 public:
  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "MyModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  // Required by MW_INFO / MW_WARN / MW_ERROR macros
  auto GetLogger() { return core_.GetLogger(); }
  ibmw::Runtime core_;
};

}  // namespace my_project
```

```cpp
// my_module.cc
#include "my_module/my_module.h"

namespace my_project {

bool MyModule::Setup(ibmw::Runtime core) {
  core_ = core;
  MW_INFO("MyModule::Setup succeeded.");
  return true;    // false = abort startup
}

bool MyModule::Run() {
  MW_INFO("MyModule::Run succeeded.");
  return true;
}

void MyModule::Stop() {
  MW_INFO("MyModule::Stop succeeded.");
}

}  // namespace my_project
```

| Lifecycle Method | When Called | Purpose |
|-----------------|------------|---------|
| `Setup(core)` | After engine init, before Run | Receive the Runtime handle; create publishers, subscribers, services |
| `Run()` | After all modules' Setup completes | Start timers, spawn threads/coroutines, publish initial data |
| `Stop()` | On engine shutdown (Ctrl-C) | Release resources, join threads |

The `Info().module_name` **must** match the YAML `modules.modules[].name`.

---

## Three Message Type Systems

IBMW supports three distinct serialization systems. You choose per-topic/service:

| | DDS IDL | ROS2 msg/srv | JSON |
|---|---------|-------------|------|
| **Schema file** | `.idl` | `.msg` / `.srv` | None (pure C++) |
| **Code generator** | `fastddsgen` | `rosidl` (ROS2 build) | None |
| **Serialization** | CDR (binary) | CDR (binary) | JSON text |
| **Zero-copy possible** | Yes (plain types) | Yes (plain types) | No |
| **Type name prefix** | `dds:` | `ros2:` | `json:` |
| **Registration** | `IBMW_DDS_TYPE_NAME` macro | Automatic via rosidl traits | `kTypeName` + `GetJsonMessageTypeSupport` |
| **Field access** | Getter/setter: `msg.num()` | Direct member: `msg.num` | Direct member: `msg.num` |
| **Extra files needed** | Type bridge `.h` + CDR impl `.cc` | None | None |
| **Best for** | High-perf, DDS interop | ROS2 ecosystem compat | Rapid prototyping, debugging |

---

## Deep Dive: DDS IDL Messages

### How DDS Code Generation Works

The full pipeline for DDS IDL types:

```
 .idl file
   |
   |  fastddsgen  (system-installed, Java-based)
   |  Flags: -flat-output-dir -no-typeobjectsupport
   |         -no-dependencies -typeros2 -cs -replace
   v
 TypeName.hpp              -- C++ struct with getter/setter methods
 TypeNameCdrAux.hpp        -- CDR serialization declarations
 TypeNameCdrAux.ipp        -- CDR serialization inline implementations
 TypeNamePubSubTypes.hpp   -- FastDDS PubSub type support (DDS 3.x)
 TypeNamePubSubTypes.cxx
   |
   |  fastcdr_v2_to_v1.py  (IBMW post-processor, auto-invoked by CMake)
   |  Converts FastCDR v2 API calls to v1 (for ROS2 Humble compatibility)
   v
 Patched .hpp / .ipp files
   |
   v
 CMake STATIC library target (ibmw::schema::<name>_idl_gencode)
```

**Key concepts:**

- **fastddsgen** generates getter/setter style types. A field `string msg`
  in IDL becomes `msg.msg()` (getter) and `msg.msg("value")` (setter) in C++.

- **CdrAux.ipp files** contain inline template functions for CDR
  serialization. To avoid ODR (One Definition Rule) violations, you must
  compile them in **exactly one** `.cc` file, then link that object to all
  targets using the type. This is the "CDR impl file" pattern.

- **Type bridge header** -- a single `.h` that includes the generated
  headers, the `.ipp` file, and calls `IBMW_DDS_TYPE_NAME` to register the
  type with IBMW's transport layer.

- **IBMW_DDS_TYPE_NAME macro** specializes `ibmw::dds::DdsTypeName<T>` so
  that `GetDdsMessageTypeSupport<T>()` knows the wire type name. Without it,
  IBMW cannot route messages of that type.

### Step-by-Step: Creating a Custom DDS Message

Suppose you want a new message type `my_project::RobotPose` with fields
`x`, `y`, `theta`.

**Step 1: Write the IDL file** (`idl/RobotPose.idl`):

```idl
module my_project {
  struct RobotPose {
    double x;
    double y;
    double theta;
  };
};
```

IDL syntax reference: modules = C++ namespaces; basic types are `long` (int32),
`unsigned long long` (uint64), `double`, `string`, `boolean`, `octet` (uint8).
Use `#include "Other.idl"` to reference types from other IDL files.

**Step 2: Add the CMake target** (in your `CMakeLists.txt`):

```cmake
find_package(ibmw REQUIRED)

# This CMake module is provided by IBMW's install
include(DdsIdlGenCode)

add_dds_idl_gencode_target(
  TARGET_NAME  robot_pose_idl_gencode
  IDL_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/idl/RobotPose.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/robot_pose
  INCLUDE_DIRS ""
  DEPENDENCIES ""
)
```

This will:
1. Call `fastddsgen` on `RobotPose.idl` at build time
2. Run `fastcdr_v2_to_v1.py` to patch for Humble compatibility
3. Create a CMake target `robot_pose_idl_gencode` with the generated headers

If your IDL `#include`s other IDL files, add their directories to
`INCLUDE_DIRS` and their targets to `DEPENDENCIES`.

**Step 3: Write the type bridge header** (`src/robot_pose_dds_types.h`):

```cpp
#pragma once

// IBMW DDS transport utilities (provides IBMW_DDS_TYPE_NAME macro)
#include "internal/typesupport/dds/dds_transport.h"

// Generated type header
#include "RobotPose.hpp"

// Generated CDR serialization (inline implementations)
#include "RobotPoseCdrAux.ipp"

// Register with IBMW: maps C++ type -> wire name
IBMW_DDS_TYPE_NAME(my_project::RobotPose, "my_project::RobotPose")
```

**Step 4: Write the CDR impl file** (`src/robot_pose_cdr_impl.cc`):

```cpp
// Compile CDR serialization for RobotPose.
// Must be compiled in exactly ONE translation unit.
#include "RobotPoseCdrAux.ipp"
```

**Step 5: CMake library for the CDR impl**:

```cmake
add_library(robot_pose_cdr STATIC src/robot_pose_cdr_impl.cc)
target_include_directories(robot_pose_cdr
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
set_target_properties(robot_pose_cdr PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(robot_pose_cdr
  PUBLIC ibmw::extensions::dds_types
         robot_pose_idl_gencode)
```

**Step 6: Use in your module**:

```cpp
#include "robot_pose_dds_types.h"  // type bridge

// Publisher:
publisher_ = core_.GetTransport().GetPublisher("pose_topic");
ibmw::transport::RegisterSendType<my_project::RobotPose>(publisher_);

my_project::RobotPose pose;
pose.x(1.0);       // setter
pose.y(2.0);
pose.theta(0.5);
ibmw::transport::Send(publisher_, pose);

// Subscriber:
auto sub = core_.GetTransport().GetSubscriber("pose_topic");
ibmw::transport::OnMessage<my_project::RobotPose>(sub,
    [](const std::shared_ptr<const my_project::RobotPose>& msg) {
      // msg->x(), msg->y(), msg->theta()  (getters)
    });
```

Link your module against `robot_pose_cdr`.

### Step-by-Step: Creating a Custom DDS RPC Service

DDS RPC uses a YAML file to describe the service, and a Python code generator
to produce C++ server/client wrapper classes.

**Step 1: Write the IDL file** with request/response structs:

```idl
// idl/MyService.idl
#include "RobotPose.idl"

module my_project {
  struct SetPoseReq {
    my_project::RobotPose pose;
    string frame_id;
  };
  struct SetPoseRsp {
    unsigned long long code;
    string msg;
  };
};
```

**Step 2: Write the service YAML** (`idl/MyService.yaml`):

```yaml
idl_file: MyService.idl
module: my_project
services:
  - name: PoseService
    methods:
      - name: SetPose
        request: SetPoseReq
        response: SetPoseRsp
```

**Step 3: Add CMake targets**:

```cmake
include(DdsIdlGenCode)
include(DdsIdlRpcGenCode)

# First: generate IDL types (request/response structs)
add_dds_idl_gencode_target(
  TARGET_NAME  my_service_idl_gencode
  IDL_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/idl/MyService.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_service
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/idl
  DEPENDENCIES robot_pose_idl_gencode
)

# Then: generate RPC wrappers (reads the YAML, produces .ibmw_rpc.dds.h/.cc)
add_dds_idl_rpc_gencode_target(
  TARGET_NAME     my_service_rpc_gencode
  SERVICE_FILE    ${CMAKE_CURRENT_SOURCE_DIR}/idl/MyService.yaml
  GENCODE_PATH    ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_service
  IDL_FILE_STEM   MyService
  DEP_IDL_TARGETS my_service_idl_gencode
)
```

The RPC code generator (`dds_idl_gen_rpc.py`) produces these classes per
service (e.g. `PoseService`):
- `PoseServiceCoServer` -- coroutine server (override method handlers)
- `PoseServiceCoClient` -- coroutine client (`co_await client.SetPose.Call(req, rsp)`)
- `PoseServiceServer` / `PoseServiceClient` -- synchronous variants
- Several other variants for different execution patterns

**Step 4: Server module**:

```cpp
#include "MyService.ibmw_rpc.dds.h"

// In Setup():
ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
ctx_ptr_->LetMe();

auto server = ctx_ptr_->CreateServer<my_project::PoseServiceCoServer>();
server->SetPose.ServeInline(
    [](ibmw::rpc::ContextRef ctx, const SetPoseReq& req, SetPoseRsp& rsp) {
      rsp.code(0);
      rsp.msg("Pose set OK");
      return ibmw::rpc::Status();
    });
```

**Step 5: Client module** (coroutine):

```cpp
// In Setup():
client_.Init(ctx_ptr_);

// In coroutine:
ctx_ptr_->LetMe();
auto status = co_await client_.SetPose.Call(req, rsp);
ctx_ptr_->LetMe();  // Required after Call() with local driver
```

### Demo 2: DDS Pub/Sub Walkthrough

This demo uses IBMW's pre-built `ExampleEventMsg` type (from
`ibmw::schema::example_idl_gencode`). The IDL definition:

```idl
module ibmw { module protocols { module example {
  struct ExampleEventMsg {
    string msg;
    long num;
  };
}; }; };
```

**Type bridge** (`src/example_event_dds_types.h`):
```cpp
#include "internal/typesupport/dds/dds_transport.h"
#include "ExampleEvent.hpp"
#include "ExampleEventCdrAux.ipp"
IBMW_DDS_TYPE_NAME(ibmw::protocols::example::ExampleEventMsg,
                    "my_project::ExampleEventMsg")
```

> **Where is `cdr_impl.cc`?** Demos 2 and 3 reuse IBMW's own schema packages
> (`ibmw::schema::example_idl_gencode` and `ibmw::schema::example_rpc_gencode`).
> When IBMW was built, those packages internally compiled their own CDR
> wrapper translation units and shipped the resulting object code inside the
> installed static archive. Therefore this template **does not** need a
> per-type `cdr_impl.cc` file for the example types -- linking against the
> schema target is enough. See [Demo 8](#demo-8-dds-pubsub-with-your-own-idl)
> for the full *bring-your-own-IDL* workflow where you do write one.

**Publisher** sends every 1 second:
```cpp
ExampleEventMsg msg;
msg.msg("Hello from IBMW template!");  // setter
msg.num(42);                            // setter
ibmw::transport::Send(publisher_, msg);
```

**Subscriber** receives via callback:
```cpp
ibmw::transport::OnMessage<ExampleEventMsg>(subscriber,
    [](const std::shared_ptr<const ExampleEventMsg>& msg) {
      // msg->msg() and msg->num() are getters
    });
```

**YAML** (`cfg/pubsub_app.yaml`):
```yaml
ibmw:
  messaging:
    drivers:
      - type: local
    publishers:
      - topic: "example_topic"
        enable_drivers: [local]
    subscribers:
      - topic: "example_topic"
        enable_drivers: [local]
  modules:
    modules:
      - name: PublisherModule
      - name: SubscriberModule
```

Use [`cfg/pubsub_app.yaml`](cfg/pubsub_app.yaml) as the maintained app configuration.

### Demo 3: DDS RPC Walkthrough

Uses IBMW's pre-built `ExampleService` with methods `GetFooData` and
`GetBarData`. The service YAML (in IBMW source):

```yaml
idl_file: ExampleRpc.idl
module: ibmw.protocols.example
services:
  - name: ExampleService
    methods:
      - name: GetFooData
        request: GetFooDataReq
        response: GetFooDataRsp
      - name: GetBarData
        request: GetBarDataReq
        response: GetBarDataRsp
```

**Server** uses `ServeInline` (handler runs on caller's thread):
```cpp
ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
ctx_ptr_->LetMe();
auto server = ctx_ptr_->CreateServer<ExampleServiceCoServer>();

server->GetFooData.ServeInline(
    [](ibmw::rpc::ContextRef ctx, const GetFooDataReq& req, GetFooDataRsp& rsp) {
      rsp.code(0);
      rsp.msg("echo " + req.msg());
      return ibmw::rpc::Status();
    });
```

**Client** uses coroutines with `co_await`:
```cpp
client_.Init(ctx_ptr_);

// In coroutine loop:
while (ctx_ptr_->Running()) {
    ctx_ptr_->LetMe();                                     // 1. Before Call
    auto status = co_await client_.GetFooData.Call(req, rsp);
    ctx_ptr_->LetMe();                                     // 2. After Call (local driver)
    // ... handle response ...
    co_await ibmw::co::ScheduleAfter(executor, 2s);       // 3. No LetMe needed here
}
```

**YAML** (`cfg/rpc_app.yaml`):
```yaml
ibmw:
  scheduling:
    executors:
      - name: time_schedule_executor
        type: thread_pool
        options: { thread_num: 1 }
      - name: work_thread_pool
        type: thread_pool
        options: { thread_num: 2 }
      - name: timeout_handle
        type: timer
        options: { bind_executor: work_thread_pool }
  service:
    drivers:
      - type: local
        options: { timeout_executor: timeout_handle }
    clients:
      - func_name: "(.*)"
        enable_drivers: [local]
    servers:
      - func_name: "(.*)"
        enable_drivers: [local]
```

Use [`cfg/rpc_app.yaml`](cfg/rpc_app.yaml) as the maintained app configuration.

---

## Deep Dive: ROS2 Messages

### How ROS2 Code Generation Works

```
 .msg / .srv file  (in a ROS2 package)
   |
   |  rosidl_generate_interfaces()  (standard ROS2 CMake macro)
   v
 C++ headers  (e.g. example_ros2/msg/ros_test_msg.hpp)
   |
   |  find_package(example_ros2)
   v
 example_ros2::example_ros2__rosidl_generator_cpp  (CMake target)

 (For RPC -- additional IBMW step):
 .srv file
   |  ros2_gen_ibmw_cpp_rpc.py  (IBMW tool)
   v
 RosTestRpc.ibmw_rpc.srv.h / .cc
   |
   v
 ibmw::schema::example_ros2_ibmw_rpc_gencode  (CMake target)
```

**Key differences from DDS**:
- No type bridge header needed -- ROS2 types auto-register via
  `rosidl_generator_traits::name<T>()`
- No CDR impl file needed
- No `IBMW_DDS_TYPE_NAME` macro needed
- Fields are **direct members** (`msg.num`), not getter/setters (`msg.num()`)
- ROS2 RPC does **not** use the Context API / `LetMe()`

### Step-by-Step: Creating a Custom ROS2 Message

Suppose you want a `my_robot_msgs/msg/WheelSpeed` message.

**Step 1: Create the ROS2 package structure**:

```
my_robot_msgs/
  CMakeLists.txt
  package.xml
  msg/
    WheelSpeed.msg
```

**Step 2: Write the .msg file** (`msg/WheelSpeed.msg`):

```
float64 left_speed
float64 right_speed
uint64  timestamp_ns
```

ROS2 msg types: `bool`, `byte`, `char`, `float32`, `float64`, `int8`,
`int16`, `int32`, `int64`, `uint8`...`uint64`, `string`.
Arrays: `float64[]` (dynamic), `float64[3]` (fixed).

**Step 3: Write `package.xml`**:

```xml
<?xml version="1.0"?>
<package format="3">
  <name>my_robot_msgs</name>
  <version>0.1.0</version>
  <description>Custom robot messages</description>
  <maintainer email="fanglinxu@huawei.com">IB-Robot</maintainer>
  <license>Apache-2.0</license>
  <buildtool_depend>ament_cmake</buildtool_depend>
  <buildtool_depend>rosidl_default_generators</buildtool_depend>
  <exec_depend>rosidl_default_runtime</exec_depend>
  <member_of_group>rosidl_interface_packages</member_of_group>
</package>
```

**Step 4: Write `CMakeLists.txt`**:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_robot_msgs)
find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/WheelSpeed.msg"
)
ament_package()
```

**Step 5: Build and use**:

The ROS2 package must be built within the IBMW source tree (as an
`add_subdirectory`) or via `colcon build`, then `find_package(my_robot_msgs)`.

```cmake
find_package(my_robot_msgs REQUIRED)
target_link_libraries(my_module
  ibmw::extensions::ros2_types
  my_robot_msgs::my_robot_msgs__rosidl_generator_cpp)
```

**Step 6: Use in your module**:

```cpp
#include "my_robot_msgs/msg/wheel_speed.hpp"
#include "internal/typesupport/ros2/ros2_transport.h"

// Publisher:
publisher_ = core_.GetTransport().GetPublisher("wheel_speed");
ibmw::transport::RegisterSendType<my_robot_msgs::msg::WheelSpeed>(publisher_);

my_robot_msgs::msg::WheelSpeed msg;
msg.left_speed = 1.5;     // Direct member access (not getter/setter)
msg.right_speed = 1.5;
msg.timestamp_ns = 123456;
ibmw::transport::Send(publisher_, msg);

// Subscriber:
ibmw::transport::OnMessage<my_robot_msgs::msg::WheelSpeed>(subscriber,
    [](const std::shared_ptr<const my_robot_msgs::msg::WheelSpeed>& msg) {
      // msg->left_speed, msg->right_speed
    });
```

### Demo 4: ROS2 Pub/Sub Walkthrough

> **Deprecated:** This demo requires `ros2_extension` (rcl runtime). For new
> projects, prefer DDS pub/sub (Demo 2) with `ros2_compatible_mode` for ROS2 interop.

Uses `example_ros2::msg::RosTestMsg`:

```
# RosTestMsg.msg
int64       num
byte[]      data
RosTestData test_msg2
```

Where `RosTestData.msg` is:
```
int32   num
float32 num2
char    data
```

This is a **non-plain** type (contains `byte[]` = `std::vector<uint8_t>`).

**Publisher**:
```cpp
#include "example_ros2/msg/ros_test_msg.hpp"

RosTestMsg msg;
msg.num = 42;
msg.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
msg.test_msg2.num = 1;
msg.test_msg2.num2 = 3.14f;
ibmw::transport::Send(publisher_, msg);
```

**CMake**:
```cmake
find_package(example_ros2 REQUIRED)
target_link_libraries(my_module
  ibmw::extensions::ros2_types
  example_ros2::example_ros2__rosidl_generator_cpp)
```

Use [`cfg/ros2_pubsub_app.yaml`](cfg/ros2_pubsub_app.yaml) as the maintained app configuration.

### Demo 5: ROS2 RPC Walkthrough

> **Deprecated:** This demo requires `ros2_extension` (rcl runtime). Use Demo 3
> (DDS RPC) with `type: dds` service driver instead.

Uses `example_ros2::srv::RosTestRpc`:
```
# RosTestRpc.srv
byte[]  data
---
int64   code
```

**Server** -- inherit the generated coroutine service class:

```cpp
#include "RosTestRpc.ibmw_rpc.srv.h"

class MyService : public example_ros2::srv::RosTestRpcCoService {
  ibmw::co::Task<ibmw::rpc::Status> RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override {
    rsp.code = static_cast<int64_t>(req.data.size());
    co_return ibmw::rpc::Status();
  }
};

// In Setup():
service_ = std::make_shared<MyService>();
core_.GetService().RegisterService(service_.get());
```

**Client** -- register + use coroutine access:

```cpp
auto rpc_handle = core_.GetService();
example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
access_ = std::make_shared<example_ros2::srv::RosTestRpcCoAccess>(rpc_handle);

// In coroutine:
auto ctx = access_->NewContextSharedPtr();
ctx->SetTimeout(std::chrono::seconds(3));
auto status = co_await access_->RosTestRpc(ctx, req, rsp);
```

**No `LetMe()` needed** -- ROS2 RPC uses the Service API, not the Context API.

Use [`cfg/ros2_rpc_app.yaml`](cfg/ros2_rpc_app.yaml) as the maintained app configuration.

---

## Deep Dive: JSON Messages

JSON is the simplest message system -- **no IDL files, no code generators,
no special tools to install**. You define plain C++ structs, provide
`to_json`/`from_json` functions (nlohmann_json convention), and add a
`kTypeName` constant.

### Step-by-Step: Creating a Custom JSON Message

**Step 1: Define your struct** (e.g. `src/my_types.h`):

```cpp
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace my_project {

struct SensorReading {
  // Required: unique type name for IBMW registration
  static constexpr const char* kTypeName = "my_project.SensorReading";

  std::string sensor_id;
  double temperature = 0.0;
  double humidity = 0.0;
  uint64_t timestamp_ms = 0;
  std::vector<double> extra_values;
};

// to_json: called during serialization (publish)
inline void to_json(nlohmann::json& j, const SensorReading& v) {
  j = nlohmann::json{
      {"sensor_id", v.sensor_id},
      {"temperature", v.temperature},
      {"humidity", v.humidity},
      {"timestamp_ms", v.timestamp_ms},
      {"extra_values", v.extra_values}};
}

// from_json: called during deserialization (subscribe)
inline void from_json(const nlohmann::json& j, SensorReading& v) {
  if (j.contains("sensor_id")) j.at("sensor_id").get_to(v.sensor_id);
  if (j.contains("temperature")) j.at("temperature").get_to(v.temperature);
  if (j.contains("humidity")) j.at("humidity").get_to(v.humidity);
  if (j.contains("timestamp_ms")) j.at("timestamp_ms").get_to(v.timestamp_ms);
  if (j.contains("extra_values")) j.at("extra_values").get_to(v.extra_values);
}

}  // namespace my_project
```

Requirements for `GetJsonMessageTypeSupport<T>()`:
1. `static constexpr const char* kTypeName` member
2. Default-constructible
3. Copy-assignable and move-assignable
4. ADL-discoverable `to_json(json&, const T&)` and `from_json(const json&, T&)`

**Step 2: Get nlohmann_json** (IBMW does not install it):

```cmake
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/archive/v3.11.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

target_link_libraries(my_module
  ibmw::api::cpp
  nlohmann_json::nlohmann_json)
```

**Step 3: Publish**:

```cpp
#include "cpp/util/json_type_support.h"
#include "my_types.h"

// In Setup():
publisher_ = core_.GetTransport().GetPublisher("sensor_topic");
publisher_.RegisterSendType(
    ibmw::json::GetJsonMessageTypeSupport<SensorReading>());

// In publish loop:
static const std::string kMsgTypeName =
    "json:" + std::string(SensorReading::kTypeName);

SensorReading msg;
msg.sensor_id = "sensor_01";
msg.temperature = 25.0;

ibmw::transport::Context ctx;
ctx.SetSerializationType("json");
publisher_.Send(kMsgTypeName, ctx, static_cast<const void*>(&msg));
```

**Step 4: Subscribe** (raw callback pattern):

```cpp
auto subscriber = core_.GetTransport().GetSubscriber("sensor_topic");
auto* ts = ibmw::json::GetJsonMessageTypeSupport<SensorReading>();

subscriber.OnMessage(ts,
    [](const ibmw_transport_context_base_t*,
       const void* msg_ptr,
       ibmw_function_base_t* release_cb) {
      // Wrap the release callback for RAII
      ibmw::transport::SubscriberReleaseCallback release(release_cb);

      auto& msg = *static_cast<const SensorReading*>(msg_ptr);
      // ... use msg ...

      release();  // MUST call to free the message buffer
    });
```

### Demo 6: JSON Pub/Sub Walkthrough

Uses `my_project::SensorReading` defined in `src/json_custom_types.h`.

The publisher sends every 1 second with temperature/humidity data.
The subscriber receives via the raw callback and calls `release()`.

Use [`cfg/json_pubsub_app.yaml`](cfg/json_pubsub_app.yaml) as the maintained app configuration.

### Demo 7: JSON RPC Walkthrough

Uses `ComputeRequest` and `ComputeResponse` for an arithmetic service
(add, multiply, subtract).

**Server** -- inherit `ServiceBase`:

```cpp
class ComputeServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  ComputeServiceImpl()
      : ServiceBase("json", "my_project.ComputeService") {
    RegisterServiceFunc(
        "Compute", nullptr,
        ibmw::json::GetJsonMessageTypeSupport<ComputeRequest>(),
        ibmw::json::GetJsonMessageTypeSupport<ComputeResponse>(),
        [](const ibmw_service_context_base_t*, const void* req_ptr,
           void* rsp_ptr, ibmw_function_base_t* callback) {
          auto& req = *static_cast<const ComputeRequest*>(req_ptr);
          auto& rsp = *static_cast<ComputeResponse*>(rsp_ptr);
          // ... compute ...
          // Signal completion:
          auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(
              callback->ops);
          ops->invoker(callback->object_buf, 0);
        });
  }
};

// In Setup():
service_ = std::make_unique<ComputeServiceImpl>();
core_.GetService().RegisterService(service_.get());
```

**Client** -- register + synchronous call via promise/future:

```cpp
std::string full_name = ibmw::rpc::GetFullFuncName(
    "json", "my_project.ComputeService", "Compute");
// Result: "json:/my_project.ComputeService/Compute"

service_ref_.RegisterClientFunc(full_name, nullptr, req_ts, rsp_ts);

// Call:
std::promise<ibmw::rpc::Status> p;
auto f = p.get_future();
service_ref_.Call(full_name, ctx, &req, &rsp,
    [&p](uint32_t code) { p.set_value(ibmw::rpc::Status(code)); });
auto status = f.get();  // Blocks until response
```

Use [`cfg/json_rpc_app.yaml`](cfg/json_rpc_app.yaml) as the maintained app configuration.

---

## Demo 8: DDS Pub/Sub With Your Own IDL

Demos 2 and 3 deliberately *avoided* writing any new IDL or CDR plumbing --
they just consume `ibmw::schema::example_idl_gencode`, which already
contains the compiled CDR code. **Demo 8 is the opposite end of the
spectrum**: it shows the complete `.idl` -> `cdr_impl.cc` -> CMake -> module
chain that you need every time you introduce a brand-new message type in
your own project.

### Files at a glance

```
src/my_msg/
  MyMsg.idl                 # Your IDL definition
  my_msg_cdr_impl.cc        # The single CDR translation unit (ODR-safe)
  my_msg_dds_types.h        # Type bridge: includes generated headers + IBMW_DDS_TYPE_NAME

src/my_msg_publisher_module/   # Sends MyMsg every second
src/my_msg_subscriber_module/  # Receives MyMsg via OnMessage callback
src/my_msg_pubsub_pkg/         # pkg-mode entry point (.so plugin)
src/my_msg_pubsub_app/         # app-mode entry point (standalone executable)

cfg/my_msg_pubsub_pkg.yaml
cfg/my_msg_pubsub_app.yaml
```

### Step 1 -- Write the IDL (`src/my_msg/MyMsg.idl`)

```idl
module my_project {
  struct MyMsg {
    string content;
    sequence<long> values;
  };
};
```

### Step 2 -- Write the CDR impl translation unit (`src/my_msg/my_msg_cdr_impl.cc`)

```cpp
// Compile MyMsg's CDR serialization in exactly ONE translation unit
// to avoid ODR (One Definition Rule) violations.
#include "MyMsgCdrAux.ipp"
```

### Step 3 -- Write the type bridge header (`src/my_msg/my_msg_dds_types.h`)

```cpp
#pragma once
#include "internal/typesupport/dds/dds_transport.h"
#include "MyMsg.hpp"
#include "MyMsgCdrAux.ipp"

// Maps the C++ type to the wire-level type name advertised by the publisher.
IBMW_DDS_TYPE_NAME(my_project::MyMsg, "my_project::MyMsg")
```

### Step 4 -- Wire it up in CMake

```cmake
find_package(ibmw REQUIRED)
include(DdsIdlGenCode)   # exposed by ibmw's CMake config

# 4a. Run fastddsgen on MyMsg.idl
add_dds_idl_gencode_target(
  TARGET_NAME  my_msg_idl_gencode
  IDL_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/my_msg/MyMsg.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_msg
  INCLUDE_DIRS ""
  DEPENDENCIES ""
)

# 4b. Bundle the IDL gencode + your CDR impl TU into a single message library.
#     Because we pass CDR_IMPLS, the helper produces a STATIC archive that
#     other targets can simply link against.
ibmw_add_dds_messages(
  TARGET       my_msg_dds
  IDL_TARGETS  my_msg_idl_gencode
  CDR_IMPLS    ${CMAKE_CURRENT_SOURCE_DIR}/src/my_msg/my_msg_cdr_impl.cc
)

# 4c. Modules just link the message library.
target_link_libraries(my_msg_publisher_module PUBLIC ibmw::api::cpp my_msg_dds)
target_link_libraries(my_msg_subscriber_module PUBLIC ibmw::api::cpp my_msg_dds)
```

> **Two helper modes.** `ibmw_add_dds_messages()` has *two* call signatures:
>
> * **Schema-shipped (mode 1)** -- omit `CDR_IMPLS` when the CDR object code
>   already lives inside the IDL package (this is what Demos 2 / 3 do with
>   IBMW's installed schemas). The helper then creates an `INTERFACE`
>   forwarding target.
> * **Bring-your-own-IDL (mode 2)** -- pass `CDR_IMPLS <files...>` whenever
>   you introduce a new IDL in your own project. The helper compiles those
>   `.cc` files into a `STATIC` archive that bundles the CDR code and the
>   generated IDL types together. Demo 8 uses this mode.

### Step 5 -- Use `MyMsg` in your modules

Publisher (`src/my_msg_publisher_module/my_msg_publisher_module.cc`):

```cpp
#include "my_msg/my_msg_dds_types.h"

publisher_ = core_.GetTransport().GetPublisher("my_msg_topic");
ibmw::transport::RegisterSendType<my_project::MyMsg>(publisher_);

my_project::MyMsg msg;
msg.content("Hello from Demo 8!");
msg.values({1, 2, 3});
ibmw::transport::Send(publisher_, msg);
```

Subscriber (`src/my_msg_subscriber_module/my_msg_subscriber_module.cc`):

```cpp
auto sub = core_.GetTransport().GetSubscriber("my_msg_topic");
ibmw::transport::OnMessage<my_project::MyMsg>(sub,
    [](const std::shared_ptr<const my_project::MyMsg>& msg) {
      // msg->content() and msg->values() are getters.
    });
```

### Step 6 -- Run

```bash
# pkg mode
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/my_msg_pubsub_pkg.yaml

# app mode
./my_msg_pubsub_app cfg/my_msg_pubsub_app.yaml
```

Demo 8 is the canonical reference whenever you copy this template to start
a new project that introduces its own message catalogue.

---

## YAML Configuration Reference

Every IBMW application requires a YAML config. Here is an annotated
example covering all sections:

```yaml
ibmw:
  # --- Logging ---
  logging:
    core_level: INFO            # Global log level: TRACE, DEBUG, INFO, WARN, ERROR
    backends:
      - type: console           # Log to stdout
      # - type: file
      #   options:
      #     file_path: /tmp/ibmw.log

  # --- Extensions (loaded before modules) ---
  extensions:
    items:
      - name: echo_extension              # Must match extension's Name()
        path: ./libibmw_echo_driver.so    # Path to .so
        options: {}

  # --- Scheduling (thread pools, timers) ---
  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool           # See Executor Types Reference
        options:
          thread_num: 2
      - name: timeout_handle
        type: timer
        options:
          bind_executor: work_thread_pool

  # --- Messaging (pub/sub transport) ---
  messaging:
    drivers:
      - type: local                 # In-process; also: dds, mqtt
    publishers:
      - topic: "example_topic"
        enable_drivers: [local]     # Which drivers carry this topic
    subscribers:
      - topic: "example_topic"
        enable_drivers: [local]

  # --- Service (RPC) ---
  service:
    drivers:
      - type: local                 # In-process; also: dds, grpc
        options:
          timeout_executor: timeout_handle
    clients:
      - func_name: "(.*)"          # Regex matching function names
        enable_drivers: [local]
    servers:
      - func_name: "(.*)"
        enable_drivers: [local]

  # --- Modules ---
  modules:
    packages:                       # Pkg mode only
      - path: ./libmy_pkg.so
    modules:
      - name: MyModule              # Must match Info().module_name
        log_level: INFO             # Per-module log level override

# --- Module custom config (accessible via core.GetConfigurator()) ---
MyModule:
  greeting: "Hello!"
  count: 42
```

Key rules:
- `modules.modules[].name` must match the module's `Info().module_name`
- For pkg mode, `modules.packages[].path` points to the `.so`
- For app mode, omit `packages` section entirely
- Topic names in publishers/subscribers must match between producer and consumer
- RPC `func_name` supports regex patterns

---

## Plain vs Non-Plain Types

| | Plain | Non-Plain |
|---|-------|-----------|
| Definition | `is_standard_layout_v<T> && is_trivially_copyable_v<T>` | Has std::string, std::vector, etc. |
| Examples | `geometry_msgs::msg::Point`, fixed-size POD structs | `std_msgs::msg::String`, `RosTestMsg` |
| DDS zero-copy | Supported (`loan_sample()`) | Not possible |
| Shared memory | Direct memcpy | Must serialize first |

Use plain types for high-frequency, real-time data. Use non-plain types
when you need variable-length data.

---

## Context API and LetMe

The Context API (`ibmw::context::Context`) is used **only by DDS RPC**
(Demo 3). ROS2 RPC and JSON RPC do not use it.

`ctx_ptr_->LetMe()` sets a thread-local variable to mark "this Context
owns the current thread." It costs one atomic increment (~nanoseconds).

**When to call LetMe():**

| Location | Required? | Why |
|----------|-----------|-----|
| `Setup()` | Yes | Establish context on the Setup thread |
| `Run()` | Yes | Run may be on a different thread |
| Coroutine loop top | Yes | co_await may resume on a different thread |
| After `co_await Call()` (local driver) | **Yes** | ServeInline overwrites the thread-local context |
| Inside ServeInline handler | No | Framework calls LetMe() for you |

**Without LetMe()**: Any operation using the context (`Running()`, `MW_INFO`,
`Send()`, `Call()`) will trigger: `"Broken context !"`.

---

## Loading Extensions

Extensions are `.so` plugins loaded before modules via YAML:

```yaml
ibmw:
  extensions:
    items:
      - name: echo_extension
        path: ./libibmw_echo_driver.so
        options: {}
```

See `cfg/extension_demo_pkg.yaml`.

---

## Switching to DDS or gRPC Drivers

All demos default to `local`. To use cross-process communication:

### DDS (same LAN)

```yaml
ibmw:
  messaging:
    drivers:
      - type: dds
        options: { domain_id: 0 }
    publishers:
      - topic: "example_topic"
        enable_drivers: [dds]
    subscribers:
      - topic: "example_topic"
        enable_drivers: [dds]
```

### gRPC (cross-host, TCP)

```yaml
ibmw:
  service:
    drivers:
      - type: grpc
        options:
          server_url: "0.0.0.0:50051"
    clients:
      - func_name: "(.*)"
        enable_drivers: [grpc]
    servers:
      - func_name: "(.*)"
        enable_drivers: [grpc]
```

### DDS RPC (pure FastDDS service calls)

```yaml
ibmw:
  service:
    drivers:
      - type: dds
        options:
          domain_id: 0
          ros2_compatible: true   # ROS2 topic naming for interop
    clients:
      - func_name: "(.*)"
        enable_drivers: [dds]
    servers:
      - func_name: "(.*)"
        enable_drivers: [dds]
```

The `type: dds` service driver uses pure FastDDS (no rcl/rclcpp). With
`ros2_compatible: true`, DDS topics and type names follow ROS2 conventions,
enabling transparent interop with native ROS2 service clients/servers.

Requires IBMW built with `-DIBMW_BUILD_DDS_EXTENSION=ON` and/or
`-DIBMW_BUILD_GRPC_EXTENSION=ON`.

---

## CMake Targets Reference

| Target | Purpose |
|--------|---------|
| `ibmw::api::cpp` | Core SDK (Module, Runtime, logging macros) |
| `ibmw::api::pkg` | Package entry point (IBMW_PKG_MAIN macro) |
| `ibmw::api::type_support` | Type support utilities (needed for JSON) |
| `ibmw::core::engine` | Full engine for app-mode executables |
| `ibmw::extensions::dds_types` | DDS type support (IBMW_DDS_TYPE_NAME, transport helpers) |
| `ibmw::extensions::ros2_types` | ROS2 type support (std/geometry/sensor msgs) |
| `ibmw::schema::example_idl_gencode` | Pre-generated ExampleEventMsg IDL types |
| `ibmw::schema::example_rpc_gencode` | Pre-generated ExampleService RPC wrappers |
| `ibmw::schema::example_ros2_ibmw_rpc_gencode` | Pre-generated RosTestRpc ROS2 RPC wrappers |

### Installed CMake Modules (for custom code generation)

After `find_package(ibmw)`, these modules are available:

| Module | Function | Purpose |
|--------|----------|---------|
| `DdsIdlGenCode` | `add_dds_idl_gencode_target()` | Generate C++ from .idl files |
| `DdsIdlRpcGenCode` | `add_dds_idl_rpc_gencode_target()` | Generate DDS RPC wrappers from YAML |
| `Ros2IdlGenCode` | (internal) | ROS2 IDL generation |
| `Ros2IbmwRpcGenCode` | `add_ros2_ibmw_rpc_gencode_target_for_one_file()` | Generate ROS2 RPC wrappers from .srv |

### Installed Tools (in `<ibmw-install>/bin/`)

| Tool | Purpose |
|------|---------|
| `ibmw` | Runtime binary for pkg mode |
| `ibmw-gen` (`ibmw_gen.py`) | Project code generator -- produces modules, configs, and CMake from `.ibmw.yaml` |
| `dds_idl_gen_rpc.py` | DDS RPC code generator (called by CMake module) |
| `ros2_gen_ibmw_cpp_rpc.py` | ROS2 RPC code generator (called by CMake module) |

---

## CMake Presets

```bash
export IBMW_INSTALL_DIR="$IBMW_BUILD_DIR"

cmake --preset default      # Debug build
cmake --build --preset default

cmake --preset release       # Release build
cmake --build --preset release
```

---

## Executor Types Reference

| YAML `type` | Description | Key `options` |
|-------------|-------------|---------------|
| `thread_pool` | Thread pool executor | `thread_num` |
| `timer` | Timer executor (for scheduling delays) | `bind_executor` |
| `strand` | Serialized execution on a parent executor | `bind_executor` |
| `dedicated` | Single dedicated thread | `thread_sched_policy` |
| `parallel` | TBB-backed parallel pool | `thread_num`, `queue_threshold` |

---

## License

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. All rights reserved.
