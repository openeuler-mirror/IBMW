# IBMW —— Intelligence Boom Middleware Framework

IBMW（Intelligence Boom Middleware）是一个面向机器人和实时系统的模块化、基于
Extension 的中间件框架。它提供统一的运行时，并以 DDS IDL / FastCDR 作为主要类型
系统，集成了可插拔的通信后端（DDS、gRPC、MQTT、TCP/UDP、ROS2）、基于 Executor
的任务调度、结构化日志、参数管理以及 RPC 框架。

**版本：** 0.1.0

## 特性

- **Extension 架构** —— 通信后端、Logger 和服务以共享库 Extension 形式在运行时
  动态加载
- **DDS 原生类型系统** —— 基于 IDL 定义的类型，使用 FastCDR 序列化；不依赖
  protobuf
- **多种通信后端** —— DDS（FastDDS）、gRPC、MQTT、TCP/UDP、local（进程内）、
  ROS2
- **Executor 框架** —— 异步任务调度，支持线程池、Strand、Timer、协程（C++20），
  以及带 CPU 亲和性的实时 Executor
- **RPC 框架** —— 基于 IDL 的请求/响应模型，支持可插拔后端及四种调用模式：
  同步、异步回调、Future、协程
- **Transport（发布/订阅）** —— 基于 Topic 的 Pub/Sub，支持后端路由
- **零拷贝支持** —— 用于共享内存传输的 LoanedMessage API
- **多 Driver Module** —— 单个 Module 可通过变参宏 `IBMW_USE_DRIVERS(...)`
  将 DDS、Iceoryx 等多个 Transport Driver 绑定到 typed slot
- **Context API** —— 模块生命周期管理与资源隔离
- **ROS2 互操作** —— 直接通过 FastDDS 与 ROS2 节点通信，无需 RCL/RMW 依赖
  （Pub/Sub 和 RPC/Service 均支持）
- **DDS RPC** —— 纯 DDS RPC Driver，通过 WriteParams/SampleInfo 实现 ROS2
  Service 通信协议，使 IBMW↔ROS2 服务调用无需 rcl/rclcpp
- **C++20** —— 协程支持、Concepts、结构化绑定

## 文档

| 文档 | 说明 |
|------|------|
| [文档索引](docs/README.md) | 面向用户、贡献者、生态资源与版本发布的公开文档入口 |
| [兼容性边界](docs/reference/compatibility.md) | ABI、零拷贝、ROS2、Action、交叉构建与依赖约束 |
| [故障排除](docs/guides/troubleshooting.md) | FastDDS/FastCDR、GLIBCXX、DDS discovery、p99、include 路径与进程清理 |
| [User Guide](docs/guides/user-guide.zh.md) | 入门指南、配置、运行 Demo、YAML 参考 |
| [Developer Guide](docs/contributing/developer-guide.zh.md) | 构建系统、代码结构、编写 Module 与 Extension |
| [AArch64 交叉编译](docs/guides/cross-compilation.md) | Linux target DDS source-prefix、sysroot 与支持边界 |
| [OHOS 小艺 Humble 交叉构建指南](docs/guides/ohos-xiaoyi-humble-cross-build-guide.zh.md) | OHOS ARM64 compile-only 指南 |
| [ROS2 Interface Import](docs/guides/ros2-interface-import-user-guide.md) | `ibmw-gen import` 的 installed/source/workspace 模式、产物与验证边界 |
| [ROS2 Action DDS-only Adapter](docs/experimental/ros2-action-dds-only-adapter-guide.md) | 实验性 Action adapter 开发路径与明确支持边界 |
| [API Reference](docs/reference/api-reference.zh.md) | 完整的 C++ SDK、C ABI、Extension 与 Package API |
| [Iceoryx Benchmark](docs/guides/transport-benchmark.zh.md) | 7-sample 零拷贝 vs DDS vs TCP 延迟教程，含一键复现脚本 |
| [生态资源](docs/ecosystem/README.md) | 面向迁移与 SDK 资产的公开索引 |
| [0.1.0 版本说明](docs/releases/0.1.0.md) | 公开能力与兼容性边界 |
| [ibmw-template](ibmw-template/) | 入门模板项目，包含 8 个 Demo，另含 Extension 加载示例，覆盖支持的消息类型系统 |
| [ibmw-gen](src/tools/ibmw_gen/) | 代码生成器 —— 从单个 `.ibmw.yaml` 生成完整项目 |

## 前置依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| 操作系统 | Linux（Ubuntu 22.04+） | |
| 编译器 | GCC 11+ 或 Clang 14+ | 需 C++20 |
| CMake | 3.24+ | |
| ROS 2 Humble | （native 可选） | Native full 构建可仅为 FastDDS/FastCDR 头文件、库与生成器元数据 source ROS 2：`sudo apt install ros-humble-fastrtps`。**不需要** rcl 运行时。AArch64 交叉构建使用[交叉编译指南](docs/guides/cross-compilation.md)中的源码构建 DDS prefix workflow。OHOS 小艺 Humble compile-only 路径见[OHOS 指南](docs/guides/ohos-xiaoyi-humble-cross-build-guide.zh.md)，不要把 host ROS 2 当作目标依赖来源。 |
| FastCDR | 2.x | 从源码构建或来自 `_deps/` |

## 构建

IBMW 使用 [CMake Presets](CMakePresets.json) 提供标准构建配置：

```bash
# Native 构建：source ROS 2 环境用于提供 FastDDS/FastCDR 头文件、库与 host 侧
# 生成器元数据，不需要 rcl 运行时。AArch64 交叉构建请使用
# docs/guides/cross-compilation.md 中的源码构建 DDS prefix workflow。
# `set +u` / `set -u` 包裹是必需的，因为 /opt/ros/humble/setup.bash
# 内部会引用未设置的 shell 变量。
set +u
source /opt/ros/humble/setup.bash
set -u

# 推荐：Full 构建（所有 Extension，无 ROS2 运行时依赖）。
# `full` preset 本身不会启用 tests / benchmarks / ROS2 interop；
# 下面 4 个 -D flag 是完整验证构建所必需的。
cmake --preset full -DIBMW_BUILD_TESTS=ON -DIBMW_BUILD_BENCHMARKS=ON \
                    -DIBMW_BUILD_WITH_ROS2=ON -DIBMW_BUILD_ROS2_EXTENSION=ON
cmake --build build -j$(nproc)

# 生成 IDL 代码 target（install 前必须执行）
cd build && make ibmw_schema_common_idl_gencode \
  ibmw_schema_geometry_idl_gencode \
  ibmw_schema_sensor_idl_gencode \
  ibmw_schema_actuator_idl_gencode \
  dds_ext_proto

# 安装（将 binaries、libs、configs 复制到 build/）
cmake --install . --prefix .

# 删除会干扰 ibmw-gen 项目的过期配置文件
rm -f ibmw-config.cmake ibmw-config-version.cmake
```

### 可用 Preset

| Preset | 说明 |
|--------|------|
| `minimal` | 仅核心运行时，开发时构建最快 |
| `dev` | 核心 + 常用 Extension + Examples（Debug） |
| `full` | **推荐。** 所有 Extension + Examples + ROS2 类型（Release） |
| `ros2` | full + ROS2 rcl 集成（**已废弃**，见下文） |
| `release` | 优化构建，不含 Examples |
| `test` | dev 构建，启用 tests |

> **ROS2 rcl 废弃通知**
>
> `ros2` Preset 和 `IBMW_BUILD_WITH_ROS2=ON` 启用的组件依赖 ROS2 rcl/rclcpp
> 运行时（`ros2_extension`、ROS2 RPC Driver）。这些组件**已废弃**，将在未来
> 版本中移除。
>
> `full` Preset 包含 `IBMW_BUILD_ROS2_TYPES=ON`，仅在构建时使用 rosidl 生成
> ROS2 类型支持（无 rcl 运行时依赖）。`ros2_interop_demo`、`ros2_point_demo`、
> `loaned_message_demo` 等 Demo 在 `full` Preset 下即可运行。
>
> IBMW 现已为所有 ROS2 互操作场景提供纯 DDS 替代方案：
>
> | 场景 | 已废弃方案（rcl） | 替代方案（纯 DDS） |
> |------|------------------|--------------------|
> | 与 ROS2 Pub/Sub | `ros2_extension` + `type: ros2` transport | `dds_extension` + `ros2_compatible: true` |
> | 与 ROS2 RPC/Service | `ros2_extension` + `type: ros2` service | `dds_extension` + `type: dds` service 配合 `ros2_compatible: true` |
> | ROS2 消息类型 | `ros2_types`（链接 rclcpp） | `dds_types` 配合自定义 IDL |
>
> `full` Preset 即可构建所需的一切，无需任何 ROS2 运行时依赖。Native
> 构建可 source `ros-humble-fastrtps` 获取 FastDDS/FastCDR 元数据；AArch64
> 交叉构建应使用源码构建 DDS prefix workflow，而不是 host ROS 2。

### 手动构建（不使用 Preset）

```bash
# Native 构建：source ROS 2 用于提供 FastDDS/FastCDR 头文件、库与 host 侧生成器元数据，
# 不需要 rcl 运行时。AArch64 交叉构建使用源码构建 DDS prefix workflow。
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

## 快速开始

构建完成后，可执行文件和配置文件位于 `build/`。设置环境：

```bash
source /opt/ros/humble/setup.bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:$LD_LIBRARY_PATH"
```

### Hello World

```bash
# Pkg 模式 —— 模块作为 .so 由 ibmw 运行时加载
./bin/ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml

# App 模式 —— 独立可执行文件
./helloworld_app ./cfg/examples_cpp_helloworld_app_mode_cfg.yaml
```

### Pub/Sub（local 后端，单进程）

```bash
./bin/ibmw run ./cfg/pubsub_single_process_cfg.yaml
```

### Pub/Sub（DDS 后端，双进程）

```bash
# Terminal 1
./bin/ibmw run ./cfg/publisher_dds_cfg.yaml

# Terminal 2
./bin/ibmw run ./cfg/subscriber_dds_cfg.yaml
```

### DDS Native（原生 DDS Pub/Sub）

```bash
./dds_native_app ./dds_native_cfg.yaml
```

### DDS Image（高吞吐 640x480 RGB @ 10Hz）

```bash
./dds_image_app ./dds_image_cfg.yaml
```

### DDS RPC（纯 DDS Service，ROS2 兼容）

这是一个 ibmw-gen Demo。先生成并构建：

```bash
cd src/samples/cpp/dds_rpc_demo
python3 ../../../tools/ibmw_gen/ibmw_gen.py dds_rpc_demo.ibmw.yaml -o dds_rpc_demo_gen
cd dds_rpc_demo_gen && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
export IBMW_BUILD_DIR="<ibmw-build-dir>"
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 设置运行时环境
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$IBMW_BUILD_DIR/lib:$IBMW_BUILD_DIR:$IBMW_BUILD_DIR/bin:$LD_LIBRARY_PATH"

# Terminal 1：Server
./dds_rpc_server_app cfg/dds_rpc_server_app.yaml

# Terminal 2：Client
./dds_rpc_client_app cfg/dds_rpc_client_app.yaml
```

完整 Demo 列表见 [User Guide](docs/guides/user-guide.zh.md)。

## 启动新项目

创建 IBMW 新项目最快的方式是使用 **ibmw-gen**：

```bash
# 1. 编写 .ibmw.yaml，定义 Module、消息和 Service
# 2. 生成
python3 src/tools/ibmw_gen/ibmw_gen.py my_project.ibmw.yaml -o my_project

# 3. 构建
cd my_project && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
export IBMW_BUILD_DIR="<ibmw-build-dir>"
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 4. 运行
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$IBMW_BUILD_DIR/lib:$IBMW_BUILD_DIR:$IBMW_BUILD_DIR/bin:$LD_LIBRARY_PATH"
./my_app cfg/my_app.yaml
```

或者复制 [ibmw-template/](ibmw-template/) 并直接修改其中的手写 Demo。完整流程
见 [ibmw-template README](ibmw-template/README.md)。

## 架构

```
ibmw/
  src/
    core/                       # 核心运行时引擎
    |  engine/                  #   子系统管理器（executor、transport、
    |  |                        #     rpc、logger、module、extension、memory、
    |  |                        #     parameter、config、watchdog、main_loop）
    |  main/                    #   入口（ibmw 二进制）
    |  extension_api/           #   Extension 开发 API
    api/                        # 面向 Module 开发者的公开接口
    |  c/                       #   C ABI 层（基于 vtable，ABI 稳定）
    |  cpp/                     #   C++ SDK（Runtime、Context、Transport、RPC、
    |  |                        #     Executor、Logger、协程工具）
    |  pkg/                     #   Package ABI（动态库入口）
    |  type_support/            #   Type Support ABI（序列化插件）
    extensions/                 # 可插拔 Extension
    |  transport/               #   DDS、Net (TCP/UDP)、MQTT、ROS2、Iceoryx、Zenoh
    |  rpc/                     #   gRPC
    |  observability/           #   Log Control、Topic Logger、OpenTelemetry
    |  management/              #   Parameter、Time Manipulator、Service
    |  |                        #     Introspection、Record/Playback
    |  utility/                 #   Echo（测试）、Proxy
    schema/                     # 类型定义
    |  dds_idl/                 #   DDS IDL 文件 + 生成的 FastCDR 代码
    |  ros2/                    #   ROS2 message/service 定义
    |  extensions/              #   Extension 专用类型定义
    samples/cpp/                # 示例应用与验证样本
    tools/                      # CLI 工具、IDL 代码生成器、ibmw-gen
  cmake/                        # 30 个 CMake 模块（依赖、代码生成）
  docs/                         # 文档
  ibmw-template/                # 外部入门项目模板，含 8 个 Demo 与 Extension 加载示例
```

### Module 生命周期

1. **Setup(Runtime)** —— Module 收到 `Runtime` 句柄，注册 Transport 类型和 RPC Service
2. **Run()** —— Module 开始正式工作（发布循环、RPC 处理、Timer）
3. **Stop()** —— Module 停止工作、释放资源、发出完成信号

### 执行模式

| 模式 | 说明 | 入口 |
|------|------|------|
| **Pkg** | 模块作为 `.so` 由 `ibmw` 运行时加载 | `./bin/ibmw run config.yaml` |
| **App** | 独立可执行文件，模块编译进可执行体 | `./my_app config.yaml` |
| **Registration** | 模块在 `main()` 中以编程方式注册 | 自定义 `main()` |

### Executor 类型

| 类型 | 说明 |
|------|------|
| `thread_pool` | 基于 Asio 的异步线程池，线程数可配置 |
| `strand` | 绑定到 `thread_pool` 的串行（有序）执行 |
| `parallel` | 基于 TBB 的并行 Executor，适合计算密集任务 |
| `dedicated` | 单一专用工作线程，自带队列 |
| `timer` | 基于时间轮的定时/周期执行 |

## 示例

所有示例位于 `src/samples/cpp/`，构建到 `build/`。

| 示例 | 说明 | 后端 | 模式 | 依赖 |
|------|------|------|------|------|
| `quickstart` | 最小化模块生命周期（Hello World） | -- | pkg, app, registration | -- |
| `executor_demo` | 线程池、Strand、Timer、协程、实时调度 | -- | pkg | -- |
| `logging_demo` | 日志级别、格式化、文件轮转、基准测试 | console, file | pkg | -- |
| `config_param_demo` | 运行时参数 get/set | -- | pkg | -- |
| `context_demo` | Context API：Executor、Logger、Transport、RPC | local | pkg | -- |
| `transport_demo` | local 与 DDS 后端的 Pub/Sub 消息 | local, DDS | pkg, app | -- |
| `dds_native_demo` | 使用手写类型的原生 DDS Pub/Sub | DDS | app | -- |
| `dds_image_demo` | 高吞吐图像流（~1MB/帧 @ 10Hz） | DDS | app | -- |
| `dds_rpc_demo` | 纯 DDS RPC Server/Client，ROS2 兼容 | DDS | app, pkg | -- |
| `loaned_message_demo` | 零拷贝 LoanedMessage API，3 种配置模式 | DDS | app | ROS2 类型（rosidl 构建期） |
| `ros2_chn` | 基于 ROS2 msg 的 Transport Pub/Sub | ROS2 (local) | pkg, app | **rcl（已废弃）** |
| `ros2_rpc` | 同步/异步/Future/协程模式的 RPC | ROS2 (local) | pkg, app | **rcl（已废弃）** |
| `ros2_interop_demo` | 无 RCL/RMW 依赖的 ROS2 互操作 | DDS | app | ROS2 类型（rosidl 构建期） |
| `ros2_point_demo` | 使用 ROS2 Plain 类型的零拷贝（geometry_msgs::Point） | DDS | app | ROS2 类型（rosidl 构建期） |
| `iceoryx_inproc_demo` | 单进程 Iceoryx Pub/Sub（RouDi 由辅助脚本管理） | Iceoryx | app | RouDi（自动启动） |
| `iceoryx_interproc_demo` | 双进程 Iceoryx Pub/Sub，共享内存零拷贝 | Iceoryx | app | RouDi（自动启动） |
| `multi_driver_demo` | 单个 Module 通过 `IBMW_USE_DRIVERS` 绑定 **两个** Driver（DDS + Iceoryx） | DDS + Iceoryx | app, pkg | DDS + Iceoryx extensions |

## Benchmark 与验证

跨 7 个 transport sample（Iceoryx、FastDDS、net_extension 共享内存等）的端到端
延迟验证，使用业界标准 6 档 payload size matrix（1KiB / 4KiB / 16KiB / 64KiB
/ 256KiB / 1MiB）。

```bash
source /opt/ros/humble/setup.bash
scripts/run_bench_sweep.sh --reset-csv
```

sweep 脚本负责每个 sample 跑前的 SHM 清理、完整 6×7 CSV 输出，详细教程见
[英文 benchmark 指南](docs/guides/transport-benchmark.md)和
[中文 benchmark 指南](docs/guides/transport-benchmark.zh.md)。

> **关于已废弃的 STATIC bench harness。** 之前由
> `IBMW_BENCH_STATIC_PAYLOAD` 控制的静态 payload bench 路径，以及配套的
> 手写 `cdr_impl.cc` shim 已被移除。所有 benchmark 现在统一走
> `transport_benchmark` payload registry（X-macro 驱动）和 `ibmw-template`
> 脚手架（方案 D-full）。

## YAML 配置

所有配置位于 `ibmw:` 顶层 key 下。模块专属配置使用与 Module 名匹配的独立顶层 key。

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

# 每个 Module 的自定义配置
MyModule:
  publish_rate_hz: 10
  output_topic: /data
```

完整 YAML 配置参考见 [User Guide](docs/guides/user-guide.zh.md)。

## Extension

| Extension | 库 | 说明 |
|-----------|----|------|
| DDS | `libibmw_dds_transport.so` | FastDDS Transport（Pub/Sub）+ 纯 DDS RPC 后端（ROS2 兼容） |
| gRPC | `libibmw_grpc_transport.so` | gRPC RPC 后端 |
| Net | `libibmw_net_transport.so` | HTTP/TCP/UDP Transport + RPC 后端 |
| MQTT | `libibmw_mqtt_transport.so` | MQTT Transport 后端 |
| ROS2 | `libibmw_ros2_transport.so` | ROS2 Transport 后端（**已废弃** —— 改用 DDS Extension） |
| Echo | `libibmw_echo_driver.so` | Echo RPC 后端（测试用） |
| Proxy | `libibmw_proxy_driver.so` | Topic 代理/转发后端 |
| Log Control | `libibmw_log_control.so` | 通过 RPC 在运行时控制日志级别 |
| Parameter | `libibmw_parameter_service.so` | 参数 get/set Service |
| Record/Playback | `libibmw_record_playback.so` | 消息录制为 bag 文件 |
| Time Manipulator | `libibmw_time_manipulator.so` | 用于测试的时间操控 |
| Topic Logger | `libibmw_topic_logger.so` | 将日志消息发布到 Topic |
| Service Introspection | `libibmw_service_introspection.so` | Service 发现与内省 |

## License

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. All rights reserved.
