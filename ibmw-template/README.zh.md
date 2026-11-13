# IBMW 外部工程模板

构建 **Intelligence Boom Robot Middleware (IBMW)** 模块与应用程序的入门工程模板，用于在 IBMW 源码树之外开发自己的项目。

本文档面向 **初次接触 IBMW** 的开发者，端到端串讲整条上手路径：安装前置工具、理解三套消息类型系统、跑通随附的 8 个 Demo（另含 Extension 加载示例），以及 —— 最关键的 —— **从零定义自己的自定义消息类型**。

---

## 目录

1.  [Demo 总览](#demo-总览)
2.  [前置条件与工具安装](#前置条件与工具安装)
3.  [Quick Start](#quick-start)
4.  [使用 ibmw-gen 的 Quick Start](#使用-ibmw-gen-的-quick-start)
5.  [工程结构](#工程结构)
6.  [两种运行模式](#两种运行模式)
7.  [编写你的第一个 Module](#编写你的第一个-module)
8.  [三套消息类型系统](#三套消息类型系统)
9.  [深入：DDS IDL 消息](#深入dds-idl-消息)
10. [深入：ROS2 消息](#深入ros2-消息)
11. [深入：JSON 消息](#深入json-消息)
12. [Demo 8：基于自定义 IDL 的 DDS Pub/Sub](#demo-8基于自定义-idl-的-dds-pubsub)
13. [YAML 配置参考](#yaml-配置参考)
14. [Plain 与 Non-Plain 类型](#plain-与-non-plain-类型)
15. [Context API 与 LetMe](#context-api-与-letme)
16. [加载 Extension](#加载-extension)
17. [切换到 DDS 或 gRPC 驱动](#切换到-dds-或-grpc-驱动)
18. [CMake Target 速查](#cmake-target-速查)
19. [CMake Presets](#cmake-presets)
20. [Executor 类型速查](#executor-类型速查)
21. [License](#license)

---

## Demo 总览

| # | Demo | 消息系统 | 演示要点 |
|---|------|----------|----------|
| 1 | 最小 Module | -- | Module 生命周期（Setup / Run / Stop），pkg + app 模式 |
| 2 | DDS Pub/Sub（schema-shipped） | DDS IDL | 复用 IBMW 预编译的 `ExampleEventMsg` —— **无需 `.idl`，无需 `cdr_impl.cc`** |
| 3 | DDS RPC（schema-shipped） | DDS IDL | 复用 IBMW 预编译的 `ExampleService` —— **无需 `.idl`，无需 `cdr_impl.cc`** |
| 4 | ROS2 Pub/Sub | ROS2 msg | 用 ROS2 消息类型做发布/订阅 **（已废弃 —— 需要 rcl）** |
| 5 | ROS2 RPC | ROS2 srv | ROS2 service 类型 RPC，含生成的协程 server 与 client **（已废弃 —— 改用 Demo 3 的 DDS RPC）** |
| 6 | JSON Pub/Sub | JSON | 自定义 C++ 结构体 + JSON 序列化，无需 IDL |
| 7 | JSON RPC | JSON | 基于 ServiceBase + ServiceRef::Call 的 JSON RPC |
| 8 | DDS Pub/Sub（自带 IDL） | DDS IDL | **从零定义 `.idl` 的端到端流程**：fastddsgen 代码生成 + 手写 `cdr_impl.cc` + type bridge |
| - | Extension 加载 | -- | 通过 YAML 配置加载 IBMW Extension（.so 插件） |

> **两种 DDS 工作流。** Demo 2 / 3 走的是 *schema-shipped* 路径：当消息类型已经存在于
> 已安装的 IBMW schema 包中（`ibmw::schema::example_idl_gencode`、
> `ibmw::schema::example_rpc_gencode`），CDR 序列化的目标代码已经被打进包内，下游
> 工程一行 `cdr_impl.cc` 都不用写。Demo 8 则演示了完整的 *自带 IDL* 路径 —— 只要你
> 在自己的工程中新增 `.idl`，就一定要走这条路。

所有 Demo 默认使用 **local** 驱动（进程内通信）。维护中的配置示例见
[`cfg/`](cfg/)。

---

## 前置条件与工具安装

### 1. C++ 编译器

GCC 11+ 或 Clang 14+，需支持 C++20。

```bash
g++ --version   # 必须 >= 11
```

### 2. CMake 3.24+

```bash
cmake --version
```

### 3. ROS 2 Humble

Demo 4 与 Demo 5 必需；同时，只要 IBMW 是用 `-DIBMW_BUILD_WITH_ROS2=ON` 构建的，
就必须装。按
[ROS 2 Humble 官方安装指南](https://docs.ros.org/en/humble/Installation.html)
安装，每次构建/运行前执行：

```bash
source /opt/ros/humble/setup.bash
```

### 4. Fast DDS 代码生成器（fastddsgen）—— 仅当你定义自定义 DDS IDL 类型时需要

> 跑 Demo 1-7 和 Extension 加载示例 **不需要** fastddsgen；它们使用 IBMW
> 预编译的 schema 类型。Demo 8 是自带 IDL 路径，因为定义了新的 `.idl`
> 消息类型，所以会用到 fastddsgen。

fastddsgen 是 eProsima 出品的 Java 工具，读取 `.idl` 文件并生成 C++ 类型定义 +
CDR 序列化代码。

**安装 JDK 11+**（fastddsgen 需要 Java）：

```bash
sudo apt install default-jdk
java -version   # 必须 >= 11
```

**安装 fastddsgen v4.x**：

```bash
git clone https://github.com/eProsima/Fast-DDS-Gen.git
cd Fast-DDS-Gen
git checkout v4.0.2
./gradlew assemble
sudo cp scripts/fastddsgen /usr/local/bin/
sudo mkdir -p /usr/local/share/fastddsgen/java
sudo cp share/fastddsgen/java/fastddsgen.jar /usr/local/share/fastddsgen/java/
```

验证：

```bash
fastddsgen --version
```

### 5. 构建并安装 IBMW

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

# 把产物 install 到 build 目录（生成 lib/ bin/ include/ share/）
cmake --install . --prefix .
```

install 完成后，`build/lib/cmake/ibmw/` 包含 CMake 配置文件，`build/bin/` 包含
辅助工具（`ibmw`、`dds_idl_gen_rpc.py`、`ros2_gen_ibmw_cpp_rpc.py`）。

---

## Quick Start

```bash
source /opt/ros/humble/setup.bash
export IBMW_BUILD_DIR="<ibmw-build-dir>"

cd ibmw-template
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# App 模式 —— 独立可执行
./pubsub_app cfg/pubsub_app.yaml

# Pkg 模式 —— 由 ibmw runner 加载的动态插件
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/pubsub_pkg.yaml
```

所有 Demo 都是常驻运行，按 Ctrl-C 退出。

---

## 使用 ibmw-gen 的 Quick Start

不必手写 module、CMake 规则与 YAML 配置，**ibmw-gen** 可以从一份
`.ibmw.yaml` 定义文件生成完整工程。

本模板自带 [`ibmw_template.ibmw.yaml`](ibmw_template.ibmw.yaml)，里面描述了全部
8 个 Demo。预生成的产物位于 [`my_ibmw_project/`](my_ibmw_project/)。

### 从零生成

```bash
# 在 ibmw-template 目录下：
export IBMW_SOURCE_DIR="<ibmw-source-dir>"
python3 "$IBMW_SOURCE_DIR/src/tools/ibmw_gen/ibmw_gen.py" \
    ibmw_template.ibmw.yaml -o my_ibmw_project
```

得到完整工程 `my_ibmw_project/`：

```
my_ibmw_project/
  CMakeLists.txt          # 可直接构建的 CMake 工程
  cfg/                    # 每个 Demo 的 YAML 配置（app + pkg）
  src/                    # module 源码；Demo 逻辑来自 yaml 的 user_code
```

### 构建并运行

```bash
source /opt/ros/humble/setup.bash
export IBMW_BUILD_DIR="<ibmw-build-dir>"
mkdir -p my_ibmw_project/build && cd my_ibmw_project/build
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# App 模式
./pubsub_app cfg/pubsub_app.yaml

# Pkg 模式
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/pubsub_pkg.yaml
```

### 为什么要用 ibmw-gen？

| 手写（本模板） | ibmw-gen |
|----------------|----------|
| 手写 module `.h`/`.cc`、CMake target、YAML 配置、type bridge | 在一份 `.ibmw.yaml` 里声明，其余全自动生成 |
| 每行代码完全可控 | 业务逻辑写在 `user_code`（或生成的 TODO 区域） |
| 适合学习 IBMW 内部机制 | 适合快速起新项目 |

两条路径产出代码等价。本模板里的手写 Demo 与生成出来的 `my_ibmw_project/`
功能完全一致。

完整 ibmw-gen 参考见
[`src/tools/ibmw_gen/README.md`](../src/tools/ibmw_gen/README.md)。

---

## 工程结构

```
ibmw-template/
  CMakeLists.txt              # 全部 8 个 Demo 的构建规则
  CMakePresets.json            # 预置构建 preset
  README.md                   # 本文件（英文版）
  README.zh.md                # 本文件
  ibmw_template.ibmw.yaml     # ibmw-gen 工程定义（覆盖 8 个 Demo）
  my_ibmw_project/             # ibmw-gen 预生成产物
  cfg/                        # YAML 配置（每个 Demo 每种模式各一份）
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
    extension_demo_pkg.yaml    #   Extension 加载
  cfg/                         # 维护中的应用与包配置
  src/
    my_module/                 # Demo 1: 最小 module
    my_module_pkg/             #   pkg 入口
    my_app/                    #   app 入口
    example_event_dds_types.h  # DDS type bridge 头文件（Demo 2-3，schema-shipped）
    publisher_module/          # Demo 2: DDS publisher
    subscriber_module/         # Demo 2: DDS subscriber
    pubsub_pkg/ pubsub_app/
    rpc_server_module/         # Demo 3: DDS RPC server
    rpc_client_module/         # Demo 3: DDS RPC client
    rpc_pkg/ rpc_app/
    my_msg/                    # Demo 8: 自带 IDL
      MyMsg.idl                #   用户自定义 IDL 文件
      my_msg_cdr_impl.cc       #   CDR impl TU（唯一编译 .ipp 的地方）
      my_msg_dds_types.h       #   type bridge 头文件
    my_msg_publisher_module/   # Demo 8: publisher
    my_msg_subscriber_module/  # Demo 8: subscriber
    my_msg_pubsub_pkg/ my_msg_pubsub_app/
    ros2_publisher_module/     # Demo 4: ROS2 publisher
    ros2_subscriber_module/    # Demo 4: ROS2 subscriber
    ros2_pubsub_pkg/ ros2_pubsub_app/
    ros2_rpc_server_module/    # Demo 5: ROS2 RPC server
    ros2_rpc_client_module/    # Demo 5: ROS2 RPC client
    ros2_rpc_pkg/ ros2_rpc_app/
    json_custom_types.h        # JSON 类型定义（Demo 6-7）
    json_publisher_module/     # Demo 6: JSON publisher
    json_subscriber_module/    # Demo 6: JSON subscriber
    json_pubsub_pkg/ json_pubsub_app/
    json_rpc_server_module/    # Demo 7: JSON RPC server
    json_rpc_client_module/    # Demo 7: JSON RPC client
    json_rpc_pkg/ json_rpc_app/
```

---

## 两种运行模式

IBMW Module 可以在两种模式下运行，运行期行为完全等价。

### Pkg 模式（动态插件）

你的 module 编译为 `.so` 共享库，由 `ibmw` 二进制根据 YAML 配置在运行期加载。

**YAML**（`cfg/my_module_pkg.yaml`）：
```yaml
ibmw:
  modules:
    packages:
      - path: ./libmy_module_pkg.so    # CMake 构建出的 .so
    modules:
      - name: MyModule
        log_level: INFO
```

**入口**（`pkg_main.cc`）：
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

字符串 `"MyModule"` **必须** 与 YAML 里 modules 列表中的 `name:` 一致。

**运行**：
```bash
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/my_module_pkg.yaml
```

### App 模式（独立可执行）

你的 module 编译进一个内嵌 IBMW 引擎的独立可执行文件，无需加载 `.so`。

**入口**（`main.cc`）：
```cpp
#include "cpp/core.h"
#include "engine/engine.h"

using namespace ibmw::runtime::core;

int32_t main(int32_t argc, char** argv) {
    Engine core;

    // 在 Initialize 之前注册 module
    my_project::PublisherModule pub;
    my_project::SubscriberModule sub;
    core.GetModuleBus().RegisterModule(pub.NativeHandle());
    core.GetModuleBus().RegisterModule(sub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    core.Initialize(options);
    core.Start();       // 阻塞直到 Shutdown()
    core.Shutdown();
    return 0;
}
```

**运行**：
```bash
./pubsub_app cfg/pubsub_app.yaml
```

### 两种模式的 CMake

```cmake
# 共用的 module 库
add_library(my_module STATIC src/my_module/my_module.cc)
target_link_libraries(my_module PUBLIC ibmw::api::cpp)
set_target_properties(my_module PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Pkg 模式（.so 插件）
add_library(my_module_pkg SHARED src/my_module_pkg/pkg_main.cc)
target_link_libraries(my_module_pkg PRIVATE ibmw::api::pkg my_module)

# App 模式（可执行）
add_executable(my_app src/my_app/main.cc)
target_link_libraries(my_app PRIVATE ibmw::core::engine my_module)
```

---

## 编写你的第一个 Module

每个 IBMW Module 都继承 `ibmw::Module`，并实现三个生命周期方法：

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
  // MW_INFO / MW_WARN / MW_ERROR 宏要求存在 GetLogger()
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
  return true;    // 返回 false 会中止启动
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

| 生命周期方法 | 触发时机 | 用途 |
|--------------|----------|------|
| `Setup(core)` | 引擎初始化后、Run 之前 | 接收 Runtime handle；创建 publisher / subscriber / service |
| `Run()` | 所有 module 的 Setup 完成后 | 启动定时器、拉起线程/协程、发布初始数据 |
| `Stop()` | 引擎关闭时（Ctrl-C） | 释放资源、join 线程 |

`Info().module_name` **必须** 与 YAML 里 `modules.modules[].name` 一致。

---

## 三套消息类型系统

IBMW 支持三套独立的序列化系统，可逐 topic / service 选用：

| | DDS IDL | ROS2 msg/srv | JSON |
|---|---------|-------------|------|
| **Schema 文件** | `.idl` | `.msg` / `.srv` | 无（纯 C++） |
| **代码生成器** | `fastddsgen` | `rosidl`（ROS2 构建） | 无 |
| **序列化** | CDR（二进制） | CDR（二进制） | JSON 文本 |
| **是否支持零拷贝** | 支持（plain 类型） | 支持（plain 类型） | 不支持 |
| **类型名前缀** | `dds:` | `ros2:` | `json:` |
| **注册方式** | `IBMW_DDS_TYPE_NAME` 宏 | rosidl traits 自动 | `kTypeName` + `GetJsonMessageTypeSupport` |
| **字段访问** | getter/setter：`msg.num()` | 直接成员：`msg.num` | 直接成员：`msg.num` |
| **额外文件** | type bridge `.h` + CDR impl `.cc` | 无 | 无 |
| **适用场景** | 高性能、与 DDS 互通 | 与 ROS2 生态对接 | 快速原型、调试 |

---

## 深入：DDS IDL 消息

### DDS 代码生成是怎么回事

DDS IDL 类型的完整流水线：

```
 .idl 文件
   |
   |  fastddsgen  （系统安装，基于 Java）
   |  Flags: -flat-output-dir -no-typeobjectsupport
   |         -no-dependencies -typeros2 -cs -replace
   v
 TypeName.hpp              -- 带 getter/setter 的 C++ struct
 TypeNameCdrAux.hpp        -- CDR 序列化声明
 TypeNameCdrAux.ipp        -- CDR 序列化的内联实现
 TypeNamePubSubTypes.hpp   -- FastDDS PubSub type support（DDS 3.x）
 TypeNamePubSubTypes.cxx
   |
   |  fastcdr_v2_to_v1.py  （IBMW 后处理脚本，由 CMake 自动调用）
   |  把 FastCDR v2 API 调用改回 v1（兼容 ROS2 Humble）
   v
 打过补丁的 .hpp / .ipp
   |
   v
 CMake STATIC 库 target（ibmw::schema::<name>_idl_gencode）
```

**几个关键概念：**

- **fastddsgen** 生成的是 getter/setter 风格类型。IDL 里的 `string msg` 字段
  在 C++ 里变成 `msg.msg()`（getter）和 `msg.msg("value")`（setter）。

- **CdrAux.ipp 文件** 装的是 CDR 序列化用的内联模板函数。为避免 ODR
  （单一定义规则）冲突，必须在 **唯一一个** `.cc` 文件里编译它，再把这个目标
  文件链给所有用到该类型的 target。这就是 "CDR impl file" 模式。

- **Type bridge 头文件** —— 单独一个 `.h`，里面 include 所生成的头文件、include
  `.ipp` 文件，并调用 `IBMW_DDS_TYPE_NAME` 把类型注册给 IBMW 的传输层。

- **IBMW_DDS_TYPE_NAME 宏** 对 `ibmw::dds::DdsTypeName<T>` 做特化，使
  `GetDdsMessageTypeSupport<T>()` 知道这个类型对应的 wire-level 类型名。少了它，
  IBMW 无法路由这种类型的消息。

### 一步步：定义自定义 DDS 消息

假设你想新增一个消息类型 `my_project::RobotPose`，含字段 `x` / `y` / `theta`。

**Step 1：写 IDL 文件**（`idl/RobotPose.idl`）：

```idl
module my_project {
  struct RobotPose {
    double x;
    double y;
    double theta;
  };
};
```

IDL 语法速查：module 对应 C++ namespace；基础类型有 `long`（int32）、
`unsigned long long`（uint64）、`double`、`string`、`boolean`、`octet`（uint8）。
要引用其他 IDL 文件里的类型，用 `#include "Other.idl"`。

**Step 2：在 `CMakeLists.txt` 里加 CMake target**：

```cmake
find_package(ibmw REQUIRED)

# 这个 CMake module 由 IBMW install 提供
include(DdsIdlGenCode)

add_dds_idl_gencode_target(
  TARGET_NAME  robot_pose_idl_gencode
  IDL_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/idl/RobotPose.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/robot_pose
  INCLUDE_DIRS ""
  DEPENDENCIES ""
)
```

它会：
1. 在构建时对 `RobotPose.idl` 调用 `fastddsgen`
2. 跑 `fastcdr_v2_to_v1.py` 打 Humble 兼容补丁
3. 创建 CMake target `robot_pose_idl_gencode`，把生成的头文件挂上来

如果你的 IDL `#include` 了其他 IDL，把对应目录加到 `INCLUDE_DIRS`，把对应
target 加到 `DEPENDENCIES`。

**Step 3：写 type bridge 头文件**（`src/robot_pose_dds_types.h`）：

```cpp
#pragma once

// IBMW DDS 传输工具（提供 IBMW_DDS_TYPE_NAME 宏）
#include "type_support/dds/dds_transport.h"

// 生成的类型头文件
#include "RobotPose.hpp"

// 生成的 CDR 序列化（内联实现）
#include "RobotPoseCdrAux.ipp"

// 注册到 IBMW：把 C++ 类型映射到 wire-level 名字
IBMW_DDS_TYPE_NAME(my_project::RobotPose, "my_project::RobotPose")
```

**Step 4：写 CDR impl 文件**（`src/robot_pose_cdr_impl.cc`）：

```cpp
// 编译 RobotPose 的 CDR 序列化代码。
// 必须在“恰好一个”TU 里编译。
#include "RobotPoseCdrAux.ipp"
```

**Step 5：CDR impl 用的 CMake 库**：

```cmake
add_library(robot_pose_cdr STATIC src/robot_pose_cdr_impl.cc)
target_include_directories(robot_pose_cdr
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
set_target_properties(robot_pose_cdr PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(robot_pose_cdr
  PUBLIC ibmw::extensions::dds_types
         robot_pose_idl_gencode)
```

**Step 6：在你的 module 里使用**：

```cpp
#include "robot_pose_dds_types.h"  // type bridge

// Publisher：
publisher_ = core_.GetTransport().GetPublisher("pose_topic");
ibmw::transport::RegisterSendType<my_project::RobotPose>(publisher_);

my_project::RobotPose pose;
pose.x(1.0);       // setter
pose.y(2.0);
pose.theta(0.5);
ibmw::transport::Send(publisher_, pose);

// Subscriber：
auto sub = core_.GetTransport().GetSubscriber("pose_topic");
ibmw::transport::OnMessage<my_project::RobotPose>(sub,
    [](const std::shared_ptr<const my_project::RobotPose>& msg) {
      // msg->x()、msg->y()、msg->theta()  （getter）
    });
```

把你的 module 链上 `robot_pose_cdr` 即可。

### 一步步：定义自定义 DDS RPC 服务

DDS RPC 用一个 YAML 文件来描述服务，再用 Python 代码生成器产出 C++ server / client
封装类。

**Step 1：写带请求/响应 struct 的 IDL 文件**：

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

**Step 2：写服务 YAML**（`idl/MyService.yaml`）：

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

**Step 3：加 CMake target**：

```cmake
include(DdsIdlGenCode)
include(DdsIdlRpcGenCode)

# 先：生成 IDL 类型（请求/响应 struct）
add_dds_idl_gencode_target(
  TARGET_NAME  my_service_idl_gencode
  IDL_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/idl/MyService.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_service
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/idl
  DEPENDENCIES robot_pose_idl_gencode
)

# 再：生成 RPC 封装（读 YAML，产出 .ibmw_rpc.dds.h/.cc）
add_dds_idl_rpc_gencode_target(
  TARGET_NAME     my_service_rpc_gencode
  SERVICE_FILE    ${CMAKE_CURRENT_SOURCE_DIR}/idl/MyService.yaml
  GENCODE_PATH    ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_service
  IDL_FILE_STEM   MyService
  DEP_IDL_TARGETS my_service_idl_gencode
)
```

RPC 代码生成器（`dds_idl_gen_rpc.py`）会为每个服务（如 `PoseService`）产出
以下类：
- `PoseServiceCoServer` —— 协程 server（重写各方法 handler）
- `PoseServiceCoClient` —— 协程 client（`co_await client.SetPose.Call(req, rsp)`）
- `PoseServiceServer` / `PoseServiceClient` —— 同步版本
- 还有若干其它执行模式的变体

**Step 4：Server module**：

```cpp
#include "MyService.ibmw_rpc.dds.h"

// Setup() 中：
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

**Step 5：Client module（协程）**：

```cpp
// Setup() 中：
client_.Init(ctx_ptr_);

// 协程内：
ctx_ptr_->LetMe();
auto status = co_await client_.SetPose.Call(req, rsp);
ctx_ptr_->LetMe();  // local 驱动下，Call() 之后必须再调一次
```

### Demo 2：DDS Pub/Sub 走读

本 Demo 使用 IBMW 预编译的 `ExampleEventMsg` 类型（来自
`ibmw::schema::example_idl_gencode`）。IDL 定义如下：

```idl
module ibmw { module protocols { module example {
  struct ExampleEventMsg {
    string msg;
    long num;
  };
}; }; };
```

**Type bridge**（`src/example_event_dds_types.h`）：
```cpp
#include "type_support/dds/dds_transport.h"
#include "ExampleEvent.hpp"
#include "ExampleEventCdrAux.ipp"
IBMW_DDS_TYPE_NAME(ibmw::protocols::example::ExampleEventMsg,
                    "my_project::ExampleEventMsg")
```

> **`cdr_impl.cc` 哪去了？** Demo 2 与 Demo 3 复用 IBMW 自己的 schema 包
> （`ibmw::schema::example_idl_gencode` 与 `ibmw::schema::example_rpc_gencode`）。
> 在 IBMW 构建时，这些包内部已经把它们的 CDR 包装 TU 编译过，并把目标代码打进了
> 已安装的 static 归档。因此本模板**不需要**为示例类型写一份逐类型的
> `cdr_impl.cc` —— 链上 schema target 即可。完整的 *自带 IDL* 流程见
> [Demo 8](#demo-8基于自定义-idl-的-dds-pubsub)。

**Publisher** 每秒发一帧：
```cpp
ExampleEventMsg msg;
msg.msg("Hello from IBMW template!");  // setter
msg.num(42);                            // setter
ibmw::transport::Send(publisher_, msg);
```

**Subscriber** 通过回调接收：
```cpp
ibmw::transport::OnMessage<ExampleEventMsg>(subscriber,
    [](const std::shared_ptr<const ExampleEventMsg>& msg) {
      // msg->msg() 与 msg->num() 是 getter
    });
```

**YAML**（`cfg/pubsub_app.yaml`）：
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

维护中的应用配置见 [`cfg/pubsub_app.yaml`](cfg/pubsub_app.yaml)。

### Demo 3：DDS RPC 走读

使用 IBMW 预编译的 `ExampleService`，含 `GetFooData` / `GetBarData` 两个方法。
其 service YAML（在 IBMW 源码内）：

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

**Server** 使用 `ServeInline`（handler 在调用方线程上执行）：
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

**Client** 使用协程 `co_await`：
```cpp
client_.Init(ctx_ptr_);

// 协程循环：
while (ctx_ptr_->Running()) {
    ctx_ptr_->LetMe();                                     // 1. Call 之前
    auto status = co_await client_.GetFooData.Call(req, rsp);
    ctx_ptr_->LetMe();                                     // 2. Call 之后（local 驱动）
    // ... 处理响应 ...
    co_await ibmw::co::ScheduleAfter(executor, 2s);       // 3. 这里不用 LetMe
}
```

**YAML**（`cfg/rpc_app.yaml`）：
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

维护中的应用配置见 [`cfg/rpc_app.yaml`](cfg/rpc_app.yaml)。

---

## 深入：ROS2 消息

### ROS2 代码生成是怎么回事

```
 .msg / .srv 文件（位于某个 ROS2 包中）
   |
   |  rosidl_generate_interfaces()  （ROS2 标准 CMake 宏）
   v
 C++ 头文件（如 example_ros2/msg/ros_test_msg.hpp）
   |
   |  find_package(example_ros2)
   v
 example_ros2::example_ros2__rosidl_generator_cpp  （CMake target）

 （RPC 还要走一步 IBMW 流程）：
 .srv 文件
   |  ros2_gen_ibmw_cpp_rpc.py  （IBMW 工具）
   v
 RosTestRpc.ibmw_rpc.srv.h / .cc
   |
   v
 ibmw::schema::example_ros2_ibmw_rpc_gencode  （CMake target）
```

**与 DDS 的关键区别**：
- 不需要 type bridge 头文件 —— ROS2 类型通过
  `rosidl_generator_traits::name<T>()` 自动注册
- 不需要 CDR impl 文件
- 不需要 `IBMW_DDS_TYPE_NAME` 宏
- 字段是 **直接成员**（`msg.num`），不是 getter/setter（`msg.num()`）
- ROS2 RPC **不**使用 Context API / `LetMe()`

### 一步步：定义自定义 ROS2 消息

假设你想定义一个 `my_robot_msgs/msg/WheelSpeed` 消息。

**Step 1：搭建 ROS2 包目录结构**：

```
my_robot_msgs/
  CMakeLists.txt
  package.xml
  msg/
    WheelSpeed.msg
```

**Step 2：写 .msg 文件**（`msg/WheelSpeed.msg`）：

```
float64 left_speed
float64 right_speed
uint64  timestamp_ns
```

ROS2 msg 类型：`bool`、`byte`、`char`、`float32`、`float64`、`int8`、`int16`、
`int32`、`int64`、`uint8`...`uint64`、`string`。
数组：`float64[]`（动态）、`float64[3]`（定长）。

**Step 3：写 `package.xml`**：

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

**Step 4：写 `CMakeLists.txt`**：

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

**Step 5：构建并使用**：

ROS2 包必须在 IBMW 源码树内（通过 `add_subdirectory`）或经 `colcon build` 构建，
之后用 `find_package(my_robot_msgs)` 引入。

```cmake
find_package(my_robot_msgs REQUIRED)
target_link_libraries(my_module
  ibmw::extensions::ros2_types
  my_robot_msgs::my_robot_msgs__rosidl_generator_cpp)
```

**Step 6：在 module 中使用**：

```cpp
#include "my_robot_msgs/msg/wheel_speed.hpp"
#include "type_support/ros2/ros2_transport.h"

// Publisher：
publisher_ = core_.GetTransport().GetPublisher("wheel_speed");
ibmw::transport::RegisterSendType<my_robot_msgs::msg::WheelSpeed>(publisher_);

my_robot_msgs::msg::WheelSpeed msg;
msg.left_speed = 1.5;     // 直接成员访问（不是 getter/setter）
msg.right_speed = 1.5;
msg.timestamp_ns = 123456;
ibmw::transport::Send(publisher_, msg);

// Subscriber：
ibmw::transport::OnMessage<my_robot_msgs::msg::WheelSpeed>(subscriber,
    [](const std::shared_ptr<const my_robot_msgs::msg::WheelSpeed>& msg) {
      // msg->left_speed、msg->right_speed
    });
```

### Demo 4：ROS2 Pub/Sub 走读

> **已废弃：** 本 Demo 需要 `ros2_extension`（rcl 运行时）。新项目优先选 DDS
> pub/sub（Demo 2）+ `ros2_compatible_mode` 实现 ROS2 互通。

使用 `example_ros2::msg::RosTestMsg`：

```
# RosTestMsg.msg
int64       num
byte[]      data
RosTestData test_msg2
```

其中 `RosTestData.msg` 为：
```
int32   num
float32 num2
char    data
```

这是一个 **non-plain** 类型（含 `byte[]` = `std::vector<uint8_t>`）。

**Publisher**：
```cpp
#include "example_ros2/msg/ros_test_msg.hpp"

RosTestMsg msg;
msg.num = 42;
msg.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
msg.test_msg2.num = 1;
msg.test_msg2.num2 = 3.14f;
ibmw::transport::Send(publisher_, msg);
```

**CMake**：
```cmake
find_package(example_ros2 REQUIRED)
target_link_libraries(my_module
  ibmw::extensions::ros2_types
  example_ros2::example_ros2__rosidl_generator_cpp)
```

维护中的应用配置见 [`cfg/ros2_pubsub_app.yaml`](cfg/ros2_pubsub_app.yaml)。

### Demo 5：ROS2 RPC 走读

> **已废弃：** 本 Demo 需要 `ros2_extension`（rcl 运行时）。请改用 Demo 3
> （DDS RPC）+ `type: dds` 服务驱动。

使用 `example_ros2::srv::RosTestRpc`：
```
# RosTestRpc.srv
byte[]  data
---
int64   code
```

**Server** —— 继承生成出的协程 service 类：

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

// Setup() 中：
service_ = std::make_shared<MyService>();
core_.GetService().RegisterService(service_.get());
```

**Client** —— 注册 + 通过协程访问：

```cpp
auto rpc_handle = core_.GetService();
example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
access_ = std::make_shared<example_ros2::srv::RosTestRpcCoAccess>(rpc_handle);

// 协程内：
auto ctx = access_->NewContextSharedPtr();
ctx->SetTimeout(std::chrono::seconds(3));
auto status = co_await access_->RosTestRpc(ctx, req, rsp);
```

**不需要 `LetMe()`** —— ROS2 RPC 用的是 Service API，不是 Context API。

维护中的应用配置见 [`cfg/ros2_rpc_app.yaml`](cfg/ros2_rpc_app.yaml)。

---

## 深入：JSON 消息

JSON 是三套消息系统里最简单的 —— **不要 IDL 文件、不要代码生成器、不要装任何
特殊工具**。你定义普通 C++ struct，提供 `to_json` / `from_json` 函数（nlohmann_json
约定），再加一个 `kTypeName` 常量即可。

### 一步步：定义自定义 JSON 消息

**Step 1：定义 struct**（如 `src/my_types.h`）：

```cpp
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace my_project {

struct SensorReading {
  // 必需：IBMW 注册用的唯一类型名
  static constexpr const char* kTypeName = "my_project.SensorReading";

  std::string sensor_id;
  double temperature = 0.0;
  double humidity = 0.0;
  uint64_t timestamp_ms = 0;
  std::vector<double> extra_values;
};

// to_json：发布时（序列化）调用
inline void to_json(nlohmann::json& j, const SensorReading& v) {
  j = nlohmann::json{
      {"sensor_id", v.sensor_id},
      {"temperature", v.temperature},
      {"humidity", v.humidity},
      {"timestamp_ms", v.timestamp_ms},
      {"extra_values", v.extra_values}};
}

// from_json：订阅时（反序列化）调用
inline void from_json(const nlohmann::json& j, SensorReading& v) {
  if (j.contains("sensor_id")) j.at("sensor_id").get_to(v.sensor_id);
  if (j.contains("temperature")) j.at("temperature").get_to(v.temperature);
  if (j.contains("humidity")) j.at("humidity").get_to(v.humidity);
  if (j.contains("timestamp_ms")) j.at("timestamp_ms").get_to(v.timestamp_ms);
  if (j.contains("extra_values")) j.at("extra_values").get_to(v.extra_values);
}

}  // namespace my_project
```

`GetJsonMessageTypeSupport<T>()` 对 T 的要求：
1. 含 `static constexpr const char* kTypeName` 成员
2. 默认可构造
3. 可拷贝赋值与移动赋值
4. ADL 可发现的 `to_json(json&, const T&)` 与 `from_json(const json&, T&)`

**Step 2：使用 IBMW SDK 提供的 nlohmann_json：

```cmake
find_package(ibmw REQUIRED)

target_link_libraries(my_module
  ibmw::api::cpp
  nlohmann_json::nlohmann_json)
```

**Step 3：发布**：

```cpp
#include "cpp/util/json_type_support.h"
#include "my_types.h"

// Setup() 中：
publisher_ = core_.GetTransport().GetPublisher("sensor_topic");
publisher_.RegisterSendType(
    ibmw::json::GetJsonMessageTypeSupport<SensorReading>());

// 发布循环：
static const std::string kMsgTypeName =
    "json:" + std::string(SensorReading::kTypeName);

SensorReading msg;
msg.sensor_id = "sensor_01";
msg.temperature = 25.0;

ibmw::transport::Context ctx;
ctx.SetSerializationType("json");
publisher_.Send(kMsgTypeName, ctx, static_cast<const void*>(&msg));
```

**Step 4：订阅**（裸回调形式）：

```cpp
auto subscriber = core_.GetTransport().GetSubscriber("sensor_topic");
auto* ts = ibmw::json::GetJsonMessageTypeSupport<SensorReading>();

subscriber.OnMessage(ts,
    [](const ibmw_transport_context_base_t*,
       const void* msg_ptr,
       ibmw_function_base_t* release_cb) {
      // 把 release 回调包成 RAII
      ibmw::transport::SubscriberReleaseCallback release(release_cb);

      auto& msg = *static_cast<const SensorReading*>(msg_ptr);
      // ... 使用 msg ...

      release();  // 必须调用，释放消息缓冲
    });
```

### Demo 6：JSON Pub/Sub 走读

使用 `src/json_custom_types.h` 中定义的 `my_project::SensorReading`。

publisher 每秒发一次，包含 temperature / humidity 数据。subscriber 通过裸回调
接收，并调用 `release()`。

维护中的应用配置见 [`cfg/json_pubsub_app.yaml`](cfg/json_pubsub_app.yaml)。

### Demo 7：JSON RPC 走读

使用 `ComputeRequest` 与 `ComputeResponse` 实现一个算术服务（add / multiply
/ subtract）。

**Server** —— 继承 `ServiceBase`：

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
          // ... 计算 ...
          // 通知完成：
          auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(
              callback->ops);
          ops->invoker(callback->object_buf, 0);
        });
  }
};

// Setup() 中：
service_ = std::make_unique<ComputeServiceImpl>();
core_.GetService().RegisterService(service_.get());
```

**Client** —— 注册 + 通过 promise/future 同步调用：

```cpp
std::string full_name = ibmw::rpc::GetFullFuncName(
    "json", "my_project.ComputeService", "Compute");
// 结果："json:/my_project.ComputeService/Compute"

service_ref_.RegisterClientFunc(full_name, nullptr, req_ts, rsp_ts);

// 调用：
std::promise<ibmw::rpc::Status> p;
auto f = p.get_future();
service_ref_.Call(full_name, ctx, &req, &rsp,
    [&p](uint32_t code) { p.set_value(ibmw::rpc::Status(code)); });
auto status = f.get();  // 阻塞直至响应到达
```

维护中的应用配置见 [`cfg/json_rpc_app.yaml`](cfg/json_rpc_app.yaml)。

---

## Demo 8：基于自定义 IDL 的 DDS Pub/Sub

Demo 2 / 3 刻意 *回避* 了写新 IDL 与 CDR 配套代码 —— 它们只消费已经把 CDR
代码编进去的 `ibmw::schema::example_idl_gencode`。**Demo 8 是另一极端**：它演示
完整的 `.idl` -> `cdr_impl.cc` -> CMake -> module 链路 —— 任何一次在你自己工程里
新增消息类型时都要走的流程。

### 涉及文件一览

```
src/my_msg/
  MyMsg.idl                 # 你的 IDL 定义
  my_msg_cdr_impl.cc        # 唯一的 CDR translation unit（ODR-safe）
  my_msg_dds_types.h        # type bridge：include 生成头文件 + IBMW_DDS_TYPE_NAME

src/my_msg_publisher_module/   # 每秒发一帧 MyMsg
src/my_msg_subscriber_module/  # 通过 OnMessage 回调接收 MyMsg
src/my_msg_pubsub_pkg/         # pkg 模式入口（.so 插件）
src/my_msg_pubsub_app/         # app 模式入口（独立可执行）

cfg/my_msg_pubsub_pkg.yaml
cfg/my_msg_pubsub_app.yaml
```

### Step 1 —— 写 IDL（`src/my_msg/MyMsg.idl`）

```idl
module my_project {
  struct MyMsg {
    string content;
    sequence<long> values;
  };
};
```

### Step 2 —— 写 CDR impl translation unit（`src/my_msg/my_msg_cdr_impl.cc`）

```cpp
// 在“恰好一个”TU 里编译 MyMsg 的 CDR 序列化代码，
// 避免 ODR（单一定义规则）冲突。
#include "MyMsgCdrAux.ipp"
```

### Step 3 —— 写 type bridge 头文件（`src/my_msg/my_msg_dds_types.h`）

```cpp
#pragma once
#include "type_support/dds/dds_transport.h"
#include "MyMsg.hpp"
#include "MyMsgCdrAux.ipp"

// 把 C++ 类型映射到 publisher 对外宣告的 wire-level 类型名。
IBMW_DDS_TYPE_NAME(my_project::MyMsg, "my_project::MyMsg")
```

### Step 4 —— 在 CMake 里把它们串起来

```cmake
find_package(ibmw REQUIRED)
include(DdsIdlGenCode)   # 由 ibmw 的 CMake 配置导出

# 4a. 对 MyMsg.idl 跑 fastddsgen
add_dds_idl_gencode_target(
  TARGET_NAME  my_msg_idl_gencode
  IDL_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/my_msg/MyMsg.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_msg
  INCLUDE_DIRS ""
  DEPENDENCIES ""
)

# 4b. 把 IDL gencode + 你的 CDR impl TU 打包成一个 message 库。
#     传入 CDR_IMPLS 时，helper 会产出 STATIC 归档，下游 target 直接链就行。
ibmw_add_dds_messages(
  TARGET       my_msg_dds
  IDL_TARGETS  my_msg_idl_gencode
  CDR_IMPLS    ${CMAKE_CURRENT_SOURCE_DIR}/src/my_msg/my_msg_cdr_impl.cc
)

# 4c. module 只需要链 message 库即可。
target_link_libraries(my_msg_publisher_module PUBLIC ibmw::api::cpp my_msg_dds)
target_link_libraries(my_msg_subscriber_module PUBLIC ibmw::api::cpp my_msg_dds)
```

> **helper 的两种调用形态。** `ibmw_add_dds_messages()` 有 *两种* 调用签名：
>
> * **Schema-shipped（mode 1）** —— 当 CDR 目标代码已经打在 IDL 包里时，省略
>   `CDR_IMPLS`（Demo 2 / 3 用 IBMW 自带 schema 时就是这种）。helper 会创建
>   一个 `INTERFACE` 转发 target。
> * **Bring-your-own-IDL（mode 2）** —— 在你自己的工程里新增 IDL 时，传入
>   `CDR_IMPLS <files...>`。helper 会把这些 `.cc` 编成一个 `STATIC` 归档，
>   把 CDR 代码与生成出的 IDL 类型打包在一起。Demo 8 走的就是这种。

### Step 5 —— 在 module 里使用 `MyMsg`

Publisher（`src/my_msg_publisher_module/my_msg_publisher_module.cc`）：

```cpp
#include "my_msg/my_msg_dds_types.h"

publisher_ = core_.GetTransport().GetPublisher("my_msg_topic");
ibmw::transport::RegisterSendType<my_project::MyMsg>(publisher_);

my_project::MyMsg msg;
msg.content("Hello from Demo 8!");
msg.values({1, 2, 3});
ibmw::transport::Send(publisher_, msg);
```

Subscriber（`src/my_msg_subscriber_module/my_msg_subscriber_module.cc`）：

```cpp
auto sub = core_.GetTransport().GetSubscriber("my_msg_topic");
ibmw::transport::OnMessage<my_project::MyMsg>(sub,
    [](const std::shared_ptr<const my_project::MyMsg>& msg) {
      // msg->content() 与 msg->values() 是 getter。
    });
```

### Step 6 —— 运行

```bash
# pkg 模式
"$IBMW_BUILD_DIR/bin/ibmw" run cfg/my_msg_pubsub_pkg.yaml

# app 模式
./my_msg_pubsub_app cfg/my_msg_pubsub_app.yaml
```

复制本模板起新项目、并要引入自有消息目录时，Demo 8 是范本参考。

---

## YAML 配置参考

每个 IBMW 应用都需要一个 YAML 配置。下面是一份覆盖所有段的注释样例：

```yaml
ibmw:
  # --- Logging ---
  logging:
    core_level: INFO            # 全局日志级别：TRACE / DEBUG / INFO / WARN / ERROR
    backends:
      - type: console           # 输出到 stdout
      # - type: file
      #   options:
      #     file_path: /tmp/ibmw.log

  # --- Extensions（在 module 之前加载）---
  extensions:
    items:
      - name: echo_extension              # 必须与 extension 的 Name() 一致
        path: ./libibmw_echo_driver.so    # .so 路径
        options: {}

  # --- Scheduling（线程池、定时器）---
  scheduling:
    executors:
      - name: work_thread_pool
        type: thread_pool           # 详见 “Executor 类型速查”
        options:
          thread_num: 2
      - name: timeout_handle
        type: timer
        options:
          bind_executor: work_thread_pool

  # --- Messaging（Pub/Sub 传输）---
  messaging:
    drivers:
      - type: local                 # 进程内；也可：dds、mqtt
    publishers:
      - topic: "example_topic"
        enable_drivers: [local]     # 该 topic 由哪些驱动承载
    subscribers:
      - topic: "example_topic"
        enable_drivers: [local]

  # --- Service（RPC）---
  service:
    drivers:
      - type: local                 # 进程内；也可：dds、grpc
        options:
          timeout_executor: timeout_handle
    clients:
      - func_name: "(.*)"          # 函数名正则匹配
        enable_drivers: [local]
    servers:
      - func_name: "(.*)"
        enable_drivers: [local]

  # --- Modules ---
  modules:
    packages:                       # 仅 pkg 模式需要
      - path: ./libmy_pkg.so
    modules:
      - name: MyModule              # 必须与 Info().module_name 一致
        log_level: INFO             # 单 module 的日志级别覆盖

# --- Module 自定义配置（通过 core.GetConfigurator() 访问）---
MyModule:
  greeting: "Hello!"
  count: 42
```

关键规则：
- `modules.modules[].name` 必须与 module 的 `Info().module_name` 一致
- pkg 模式下，`modules.packages[].path` 指向 `.so`
- app 模式下，整段 `packages` 直接省略
- publisher / subscriber 的 topic 名必须在生产端与消费端保持一致
- RPC 的 `func_name` 支持正则模式

---

## Plain 与 Non-Plain 类型

| | Plain | Non-Plain |
|---|-------|-----------|
| 定义 | `is_standard_layout_v<T> && is_trivially_copyable_v<T>` | 含 std::string、std::vector 等 |
| 例子 | `geometry_msgs::msg::Point`、定长 POD struct | `std_msgs::msg::String`、`RosTestMsg` |
| DDS 零拷贝 | 支持（`loan_sample()`） | 不支持 |
| 共享内存 | 直接 memcpy | 必须先序列化 |

高频实时数据用 plain 类型；变长数据再考虑 non-plain。

---

## Context API 与 LetMe

Context API（`ibmw::context::Context`）**仅** DDS RPC（Demo 3）使用。
ROS2 RPC 与 JSON RPC 都不用它。

`ctx_ptr_->LetMe()` 设置一个 thread-local 变量，标记 “当前线程归这个 Context
所有”。开销是一次原子自增（纳秒级）。

**何时调用 LetMe()：**

| 位置 | 是否必需 | 原因 |
|------|----------|------|
| `Setup()` | 是 | 在 Setup 线程上建立 context |
| `Run()` | 是 | Run 可能跑在另一个线程 |
| 协程循环顶部 | 是 | co_await 后可能在另一个线程恢复 |
| `co_await Call()` 之后（local 驱动） | **是** | ServeInline 会覆盖 thread-local context |
| ServeInline handler 内部 | 否 | 框架会替你调 LetMe() |

**没有 LetMe()**：任何使用 context 的操作（`Running()`、`MW_INFO`、`Send()`、
`Call()`）都会报错 `"Broken context !"`。

---

## 加载 Extension

Extension 是 `.so` 插件，在 module 之前由 YAML 加载：

```yaml
ibmw:
  extensions:
    items:
      - name: echo_extension
        path: ./libibmw_echo_driver.so
        options: {}
```

参见 `cfg/extension_demo_pkg.yaml`。

---

## 切换到 DDS 或 gRPC 驱动

所有 Demo 默认用 `local`。要做跨进程通信：

### DDS（同 LAN）

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

### gRPC（跨主机，TCP）

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

### DDS RPC（纯 FastDDS 服务调用）

```yaml
ibmw:
  service:
    drivers:
      - type: dds
        options:
          domain_id: 0
          ros2_compatible: true   # 用 ROS2 topic 命名以便互通
    clients:
      - func_name: "(.*)"
        enable_drivers: [dds]
    servers:
      - func_name: "(.*)"
        enable_drivers: [dds]
```

`type: dds` 服务驱动使用纯 FastDDS（不带 rcl/rclcpp）。打开
`ros2_compatible: true` 后，DDS topic 与类型名遵循 ROS2 约定，可与原生 ROS2
service client / server 透明互通。

要求 IBMW 在构建时打开了 `-DIBMW_BUILD_DDS_EXTENSION=ON` 与/或
`-DIBMW_BUILD_GRPC_EXTENSION=ON`。

---

## CMake Target 速查

| Target | 用途 |
|--------|------|
| `ibmw::api::cpp` | 核心 SDK（Module、Runtime、日志宏） |
| `ibmw::api::pkg` | Pkg 入口（IBMW_PKG_MAIN 宏） |
| `ibmw::api::type_support` | type support 工具（JSON 必需） |
| `ibmw::core::engine` | App 模式可执行所需的完整引擎 |
| `ibmw::extensions::dds_types` | DDS type support（IBMW_DDS_TYPE_NAME、传输辅助函数） |
| `ibmw::extensions::ros2_types` | ROS2 type support（std/geometry/sensor msgs） |
| `ibmw::schema::example_idl_gencode` | 预生成的 ExampleEventMsg IDL 类型 |
| `ibmw::schema::example_rpc_gencode` | 预生成的 ExampleService RPC 封装 |
| `ibmw::schema::example_ros2_ibmw_rpc_gencode` | 预生成的 RosTestRpc ROS2 RPC 封装 |

### 自定义代码生成可用的 CMake 模块

`find_package(ibmw)` 之后即可使用：

| 模块 | 函数 | 用途 |
|------|------|------|
| `DdsIdlGenCode` | `add_dds_idl_gencode_target()` | 从 .idl 生成 C++ |
| `DdsIdlRpcGenCode` | `add_dds_idl_rpc_gencode_target()` | 从 YAML 生成 DDS RPC 封装 |
| `Ros2IdlGenCode` | （内部） | ROS2 IDL 生成 |
| `Ros2IbmwRpcGenCode` | `add_ros2_ibmw_rpc_gencode_target_for_one_file()` | 从 .srv 生成 ROS2 RPC 封装 |

### 已安装工具（位于 `<ibmw-install>/bin/`）

| 工具 | 用途 |
|------|------|
| `ibmw` | pkg 模式的运行期二进制 |
| `ibmw-gen`（`ibmw_gen.py`） | 工程代码生成器 —— 从 `.ibmw.yaml` 产出 module、配置与 CMake |
| `dds_idl_gen_rpc.py` | DDS RPC 代码生成器（由 CMake 模块调用） |
| `ros2_gen_ibmw_cpp_rpc.py` | ROS2 RPC 代码生成器（由 CMake 模块调用） |

---

## CMake Presets

```bash
export IBMW_INSTALL_DIR="$IBMW_BUILD_DIR"

cmake --preset default      # Debug 构建
cmake --build --preset default

cmake --preset release       # Release 构建
cmake --build --preset release
```

---

## Executor 类型速查

| YAML `type` | 描述 | 关键 `options` |
|-------------|------|----------------|
| `thread_pool` | 线程池 executor | `thread_num` |
| `timer` | 定时器 executor（用于延迟调度） | `bind_executor` |
| `strand` | 在父 executor 上顺序执行 | `bind_executor` |
| `dedicated` | 单个独占线程 | `thread_sched_policy` |
| `parallel` | TBB 支撑的并行池 | `thread_num`、`queue_threshold` |

---

## License

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. All rights reserved.
