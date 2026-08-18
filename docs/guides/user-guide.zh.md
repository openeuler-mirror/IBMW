# IBMW User Guide（用户指南）

本指南涵盖 IBMW 入门、配置、运行 Demo 以及完整的 YAML 配置参考。

## 目录

1. [入门](#入门)
2. [构建选项](#构建选项)
3. [运行 Demo](#运行-demo)
4. [Demo 参考](#demo-参考)
5. [YAML 配置参考](#yaml-配置参考)
6. [故障排查](#故障排查)

---

## 入门

### 安装

```bash
# 安装前置依赖（Ubuntu 22.04+）
sudo apt update
sudo apt install build-essential cmake gcc-11 g++-11
# Native FastDDS/FastCDR 元数据（不需要 rcl 运行时）。AArch64 交叉构建请使用
# docs/guides/cross-compilation.md 中的源码构建 DDS prefix workflow。OHOS 小艺 Humble
# compile-only 路径见 docs/guides/ohos-xiaoyi-humble-cross-build-guide.zh.md，不要用
# host ROS 2 作为目标依赖来源。
sudo apt install ros-humble-fastrtps

# Native 构建：source ROS 2 用于提供 FastDDS/FastCDR 头文件、库与 host 侧生成器元数据。
# /opt/ros/humble/setup.bash 内部会引用未设置变量，因此需要 set +u / set -u。
set +u
source /opt/ros/humble/setup.bash
set -u

# 推荐的 rcl-free 构建：DDS transport + 构建期 ROS2 消息类型。
# 此处不要启用已废弃的 IBMW_BUILD_WITH_ROS2/IBMW_BUILD_ROS2_EXTENSION。
cmake --preset full -DIBMW_BUILD_TESTS=ON -DIBMW_BUILD_BENCHMARKS=ON \
                    -DIBMW_BUILD_DDS_EXTENSION=ON -DIBMW_BUILD_ROS2_TYPES=ON \
                    -DIBMW_BUILD_WITH_ROS2=OFF -DIBMW_BUILD_ROS2_EXTENSION=OFF
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

> **注意：** `full` Preset 构建所有 Extension 与 Examples，且不依赖任何 ROS2
> rcl 运行时。如果你需要已废弃的 ROS2 rcl 集成，使用 `cmake --preset ros2`
> （需完整的 ROS2 安装）。
> AArch64 DDS 交叉构建使用 `docs/guides/cross-compilation.md` 中的源码构建 DDS prefix
> workflow。若目标是 OHOS 小艺 Humble 环境，请继续参考
> [OHOS 小艺 Humble 交叉构建指南](ohos-xiaoyi-humble-cross-build-guide.zh.md)。
> 不要把 host `/opt/ros/humble` 当作目标依赖来源。

### 运行你的第一个 Module

```bash
source /opt/ros/humble/setup.bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:$LD_LIBRARY_PATH"

# 运行 Hello World 示例
./bin/ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml
```

你应当看到：

```
IBMW v0.1.0 starting...
Info ... [core] IBMW v0.1.0 | IB Middleware Framework
Info ... [HelloWorldModule] Setup
Info ... [HelloWorldModule] Run
```

按 `Ctrl+C` 关闭。

### 三种执行模式

IBMW 支持三种 Module 运行方式：

**Pkg 模式** —— `ibmw` 二进制在运行时加载 Module 共享库：
```bash
./bin/ibmw run ./cfg/my_config.yaml
```

**App 模式** —— Module 编译进可执行体的独立程序：
```bash
./my_app ./cfg/my_config.yaml
```

**Registration 模式** —— Module 在 `main()` 中以编程方式注册：
```bash
./my_app_registration_mode
```

### 使用 ibmw-gen 启动新项目

要无需手写样板代码即可引导新项目，使用 **ibmw-gen**。在一个 `.ibmw.yaml`
文件中定义 Module、消息和 Service，然后生成完整项目：

```bash
# 1. 从 .ibmw.yaml 生成项目
python3 src/tools/ibmw_gen/ibmw_gen.py my_project.ibmw.yaml -o my_project

# 2. 构建生成的项目
source /opt/ros/humble/setup.bash
cd my_project && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
export IBMW_BUILD_DIR="<ibmw-build-dir>"
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 3. 运行
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$IBMW_BUILD_DIR/lib:$IBMW_BUILD_DIR:$IBMW_BUILD_DIR/bin:$LD_LIBRARY_PATH"
./my_app cfg/my_app.yaml
```

> **重要：** 将 `$IBMW_BUILD_DIR` 设为实际的 IBMW 构建目录。`LD_LIBRARY_PATH` 必须包含 `build/lib`、
> `build/`、`build/bin`，因为 IBMW 将共享库与可执行文件分别安装到这些子目录。

ibmw-gen 会生成 Module 脚手架、CMake 规则、YAML 配置以及 DDS/JSON 类型定义。
你只需在 TODO 标记处填入业务逻辑。

要查看含 8 个可运行 Demo（另含 Extension 加载示例）的实战教程，参见 [ibmw-template/](../../ibmw-template/)
和 [ibmw-gen 参考](../../src/tools/ibmw_gen/README.zh.md)。

---

## 构建选项

所有构建选项是 CMake 变量，使用 `-D` 设置：

### 核心选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `IBMW_BUILD_RUNTIME` | ON | 构建核心运行时引擎 |
| `IBMW_BUILD_EXAMPLES` | OFF | 构建示例应用 |
| `IBMW_BUILD_TESTS` | OFF | 构建单元测试 |
| `IBMW_BUILD_CLI_TOOLS` | OFF | 构建 `ibmw` CLI 工具 |
| `IBMW_BUILD_WITH_ROS2` | OFF | 启用 ROS2 rcl 集成**（已废弃）** |
| `IBMW_BUILD_PROTOCOLS` | ON | 构建 DDS IDL 协议定义 |
| `IBMW_USE_FMT_LIB` | ON | 使用 fmt 库进行格式化 |
| `IBMW_ENABLE_DLOPEN_DEEPBIND` | ON | 插件加载使用 RTLD_DEEPBIND |

### Extension 选项

所有 Extension 默认 OFF，且依赖 `IBMW_BUILD_RUNTIME=ON`：

| 选项 | Extension |
|------|-----------|
| `IBMW_BUILD_DDS_EXTENSION` | DDS Transport + RPC（FastDDS） |
| `IBMW_BUILD_NET_EXTENSION` | HTTP/TCP/UDP Transport + RPC |
| `IBMW_BUILD_MQTT_EXTENSION` | MQTT Transport |
| `IBMW_BUILD_ICEORYX_EXTENSION` | Iceoryx 共享内存 Transport（零拷贝） |
| `IBMW_BUILD_GRPC_EXTENSION` | gRPC RPC |
| `IBMW_BUILD_ROS2_EXTENSION` | ROS2 Transport **（已废弃）**—— 改用 DDS Extension |
| `IBMW_BUILD_ECHO_EXTENSION` | Echo RPC（测试用） |
| `IBMW_BUILD_PROXY_EXTENSION` | Topic 代理/转发 |
| `IBMW_BUILD_PARAMETER_EXTENSION` | Parameter Service |
| `IBMW_BUILD_LOG_CONTROL_EXTENSION` | 运行时日志级别控制 |
| `IBMW_BUILD_RECORD_PLAYBACK_EXTENSION` | 消息录制/回放 |
| `IBMW_BUILD_TIME_MANIPULATOR_EXTENSION` | 时间操控 |
| `IBMW_BUILD_TOPIC_LOGGER_EXTENSION` | 基于 Topic 的日志输出 |
| `IBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION` | Service 发现 |

---

## 运行 Demo

### 通用环境配置

```bash
source /opt/ros/humble/setup.bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:$LD_LIBRARY_PATH"
```

### 使用 Shell 脚本

大多数 Demo 在 `build/` 中提供了 shell 脚本：

```bash
bash start_examples_cpp_helloworld.sh
bash start_examples_cpp_executor.sh
bash start_pubsub_local.sh
```

### 使用 ibmw 二进制（Pkg 模式）

```bash
./bin/ibmw run ./cfg/<config_name>.yaml
```

### 使用独立 App（App 模式）

```bash
./<app_name> ./cfg/<config_name>.yaml
```

---

## Demo 参考

### 1. quickstart —— Hello World

**目的：** 最小化模块生命周期（`Setup` -> `Run` -> `Stop`）。

**演示内容：**
- Module 生命周期钩子
- 每个 Module 的 YAML 配置访问
- `MW_INFO()` 日志宏
- 三种执行模式（Pkg、App、Registration）

**运行方式：**
```bash
# Pkg 模式
./bin/ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml

# App 模式
./helloworld_app ./cfg/examples_cpp_helloworld_app_mode_cfg.yaml

# Registration 模式（无需 config）
./helloworld_app_registration_mode
```

**所需 Extension：** 无（仅核心）。

---

### 2. executor_demo —— 任务调度

**目的：** 演示 Executor 框架：线程池、Strand Executor、定时分发、Timer、
协程以及实时调度。

**演示内容：**
- `core_.GetExecutor().GetExecutor("name")` —— 获取 Executor 句柄
- `executor.Dispatch(task)` / `executor.DispatchAfter(duration, task)`
- Strand Executor 实现串行（线程安全）执行
- `ibmw::executor::CreateTimer(executor, interval, callback)` —— 周期 Timer
- `co_await co::Schedule(executor)` —— 协程调度
- 实时：`SCHED_FIFO`、`SCHED_RR` 调度策略 + CPU 绑定

**运行方式：**
```bash
bash start_examples_cpp_executor.sh           # 基础
bash start_examples_cpp_executor_timer.sh     # Timer
bash start_examples_cpp_executor_co.sh        # 协程
bash start_examples_cpp_executor_co_loop.sh   # 协程循环
bash start_examples_cpp_executor_real_time.sh # 实时（需要权限）
```

**所需 Extension：** 无（仅核心）。

---

### 3. logging_demo —— 日志系统

**目的：** 演示所有日志级别、自定义格式化、文件轮转、同步写入和基准测试。

**演示内容：**
- `MW_TRACE/MW_DEBUG/MW_INFO/MW_WARN/MW_ERROR/MW_FATAL()` 宏
- Console 与 file 日志后端
- 按大小轮转日志文件
- Per-module 日志级别
- 日志吞吐基准测试

**运行方式：**
```bash
bash start_examples_cpp_logger.sh                       # 所有级别
bash start_examples_cpp_logger_format.sh                # 自定义格式
bash start_examples_cpp_logger_rotate_file.sh           # 文件轮转
bash start_examples_cpp_logger_rotate_file_with_sync.sh # 同步写入
bash start_examples_cpp_logger_bench.sh                 # 基准测试
```

**所需 Extension：** 无（仅核心）。

---

### 4. config_param_demo —— Parameter

**目的：** 演示运行时 Parameter API（Key-Value 存储）。

**演示内容：**
- `core_.GetParams()` —— Parameter 句柄
- `SetParameter(key, value)` / `GetParameter(key)`
- 多线程并发参数访问

**运行方式：**
```bash
bash start_examples_cpp_parameter.sh
```

**所需 Extension：** 无（仅核心）。

---

### 5. context_demo —— Context API

**目的：** 演示高层 Context API，用于资源托管的 Module 开发，含 Transport、
RPC、Logging、Executor 访问。

**演示内容：**
- `ibmw::context::Context(core)` —— 创建 Context
- `ctx_ptr_->LetMe()` —— 线程局部 Context 所有权
- 通过 Context 使用 Publisher/Subscriber（inline 与 on-executor 回调）
- 通过 Context 使用 RPC Client/Server
- `ibmw::context::Running()` —— 生命周期感知的循环控制

**运行方式（8 个子 Demo）：**
```bash
bash start_examples_cpp_context_executor.sh
bash start_examples_cpp_context_logger.sh

# Transport（在不同终端运行 publisher + subscriber）
bash start_examples_cpp_context_publisher.sh
bash start_examples_cpp_context_subscriber_inline.sh

# RPC（在不同终端运行 client + server）
bash start_examples_cpp_context_rpc_client.sh
bash start_examples_cpp_context_rpc_server_inline.sh
```

**所需 Extension：** 无（local Transport）。

---

### 6. transport_demo —— Pub/Sub

**目的：** 演示 local 与 DDS 后端的 Pub/Sub 消息。

**演示内容：**
- `core_.GetTransport().GetPublisher("topic")` / `GetSubscriber("topic")`
- `ibmw::transport::RegisterSendType<T>()` / `ibmw::transport::Send()`
- `ibmw::transport::OnMessage<T>(subscriber, callback)`
- local vs DDS 后端路由
- 跨进程 DDS 通信

**运行方式：**
```bash
# Local 后端（单进程）
bash start_pubsub_local.sh

# DDS 后端（单进程）
bash start_pubsub_dds.sh

# DDS 后端（双进程）
bash start_publisher_dds.sh   # Terminal 1
bash start_subscriber_dds.sh  # Terminal 2
```

**所需 Extension：** DDS（DDS Demo）；local Demo 无需。

---

### 7. dds_native_demo —— 原生 DDS 类型

**目的：** 直接 DDS Pub/Sub，使用手写类型（无 IDL 代码生成）。

**演示内容：**
- 使用 `IBMW_DDS_TYPE_NAME()` 宏定义自定义 DDS 类型
- 手写 FastCDR serialize/deserialize 函数
- 单进程 DDS 回环

**运行方式：**
```bash
./dds_native_app ./dds_native_cfg.yaml
```

**所需 Extension：** DDS。

---

### 8. dds_image_demo —— 高吞吐消息

**目的：** 演示大 payload DDS 消息（640x480 RGB 图像 @ 10Hz）。

**演示内容：**
- 每条消息 ~1MB，持续吞吐 ~10MB/s
- 大 payload 亚毫秒级延迟
- 合成 RGB 渐变测试图

**运行方式：**
```bash
./dds_image_app ./dds_image_cfg.yaml
```

**所需 Extension：** DDS。

---

### 9. loaned_message_demo —— 零拷贝

**目的：** LoanedMessage API 端到端验证（零拷贝发布）。

**演示内容：**
- `BorrowLoanedMessage<T>()` / `SendLoaned()` —— 零拷贝发布
- Plain 类型（通过 SHM 零拷贝）vs 非 Plain 类型（堆内存兜底）
- 三种配置模式：AUTO、STRICT、COMPAT
- 零拷贝统计跟踪

**运行方式：**
```bash
# 完整端到端测试
./loaned_message_app ./cfg/loaned_message_cfg.yaml

# 配置模式测试（6 个自动化场景）
./config_test_app ./cfg/config_test_cfg.yaml
```

**所需 Extension：** DDS + ROS2（仅 rosidl 构建期；无 rcl 运行时）。

> **注意：** 需要 `full` CMake Preset（或更高），启用
> `IBMW_BUILD_ROS2_TYPES=ON`。运行时无 rcl 依赖（纯 FastDDS Transport）。

---

### 10. ros2_chn —— ROS2 Transport **（已废弃）**

**目的：** 使用 local 后端演示基于 ROS2 message 的 Transport Pub/Sub。

**演示内容：**
- ROS2 `.msg` 协议类型（`example_ros2::msg::RosTestMsg`）
- 用 ROS2 类型调用 `ibmw::transport::RegisterSendType<T>()`
- Pkg 模式（双 package）和 App 模式变体

**运行方式：**
```bash
bash start_examples_cpp_ros2_chn.sh               # 双 package
bash start_examples_cpp_ros2_chn_single_pkg.sh     # 单 package
bash start_examples_cpp_ros2_chn_publisher_app.sh  # App 模式 publisher
bash start_examples_cpp_ros2_chn_subscriber_app.sh # App 模式 subscriber
```

**所需 Extension：** ROS2（local 后端）。

> **已废弃：** `ros2_chn` 需要运行时依赖 rcl 的 `ros2_extension`。新项目请使用
> DDS Extension（`dds_ext`）。需用 `ros2` CMake Preset 才能构建此 Demo。

---

### 11. ros2_rpc —— RPC 模式 **（已废弃 —— 改用 dds_rpc_demo）**

**目的：** 全面 RPC 演示，含覆盖所有调用模式的 7 个变体。

**演示内容：**
- 同步 RPC：`SyncAccess` —— 带超时的阻塞调用
- 异步 RPC：`AsyncAccess` —— 基于回调
- Future RPC：`FutureAccess` —— 基于 `std::future`
- 协程 RPC：`CoAccess` —— 基于 `co_await`
- RPC filter/middleware 用于日志和计时
- 通过 `ctx_ptr->SetTimeout(3s)` 设置 per-call 超时

**运行方式：**
```bash
bash start_examples_cpp_ros2_rpc_sync.sh       # 同步
bash start_examples_cpp_ros2_rpc_async.sh      # 异步
bash start_examples_cpp_ros2_rpc_future.sh     # Future
bash start_examples_cpp_ros2_rpc_co.sh         # 协程 + filter
bash start_examples_cpp_ros2_rpc_single_pkg.sh # 单 package
bash start_examples_cpp_ros2_rpc_server_app.sh # App 模式 server
bash start_examples_cpp_ros2_rpc_client_app.sh # App 模式 client
```

**所需 Extension：** ROS2（local RPC）。

> **已废弃：** `ros2_rpc` 使用的 `type: ros2` Service Driver 运行时依赖 rcl。
> 改用 `type: dds` Service Driver，并参见 `dds_rpc_demo` 推荐的纯 DDS 方案。
> 需用 `ros2` CMake Preset 才能构建此 Demo。

---

### 12. ros2_interop_demo —— ROS2 互操作

**目的：** 演示通过 FastDDS 与 ROS2 节点双向通信，无需任何 RCL/RMW 依赖。

**演示内容：**
- `ibmw::ros2::transport::RegisterSendType<T>()` —— ROS2 兼容类型注册
- `ros2_compatible_mode: true` —— 使用 `rt/` 前缀的 ROS2 Topic 命名
- 双向：`ros2 topic echo /chatter` 可看到 IBMW 发布的消息
- 标准 ROS2 类型（String、Int32、Point、Pose、Twist）

**运行方式：**
```bash
# Terminal 1：IBMW 节点
./ros2_interop_app ./cfg/ros2_interop_cfg.yaml

# Terminal 2：用 ROS2 CLI 验证
ros2 topic list
ros2 topic echo /chatter

# Terminal 3：ROS2 talker
ros2 run demo_nodes_cpp talker
```

**所需 Extension：** DDS + ROS2（仅 rosidl 构建期；无 rcl 运行时）。

> **注意：** 需要 `full` CMake Preset（或更高），启用
> `IBMW_BUILD_ROS2_TYPES=ON`。运行时无 rcl 依赖（纯 FastDDS Transport）。

---

### 13. ros2_point_demo —— Plain 类型零拷贝

**目的：** 演示与 ROS2 Plain 类型（`geometry_msgs::msg::Point`）的零拷贝
通信。

**演示内容：**
- Plain 类型检测（`trivially_copyable` + `standard_layout`）
- 通过 DDS `loan_sample` 自动启用零拷贝优化
- 与 ROS2 互操作：`ros2 topic echo /rt/point_topic`

**运行方式：**
```bash
./ros2_point_app ./cfg/ros2_point_cfg.yaml

# 在另一个终端验证：
ros2 topic echo /rt/point_topic geometry_msgs/msg/Point
```

**所需 Extension：** DDS + ROS2（仅 rosidl 构建期；无 rcl 运行时）。

> **注意：** 需要 `full` CMake Preset（或更高），启用
> `IBMW_BUILD_ROS2_TYPES=ON`。运行时无 rcl 依赖（纯 FastDDS Transport）。

---

### 14. dds_rpc_demo —— 纯 DDS RPC（原生 .srv，ROS2 兼容）

**目的：** 演示 DDS RPC Service Driver（`type: dds`）使用原生 `.srv` 模式
进行跨进程 RPC（基于 FastDDS）。业务类型由 FastDDS 直接序列化/反序列化（无
信封包装），实现与 ROS2 标准 service 100% 线协议兼容互操作。

**演示内容：**
- `type: dds` Service Driver 使用原生 `.srv` TypeSupport
- 纯 DDS RPC —— 无需 rcl/rclcpp，仅需 FastDDS
- 100% ROS2 线协议兼容：`ros2 service call` 与 `ros2 service list` 可直接使用
- ibmw-gen 中的 `srv:` 字段自动派生 ROS2 DDS 类型名
- 双向 ROS2 互操作（IBMW server ↔ ROS2 client，ROS2 server ↔ IBMW client）
- 基于协程（`co_await`）的 RPC client 与 server 模式
- ibmw-gen 端到端 DDS RPC 项目代码生成

**运行方式：**
```bash
# 从 .ibmw.yaml 生成
cd src/samples/cpp/dds_rpc_demo
python3 <ibmw>/src/tools/ibmw_gen/ibmw_gen.py dds_rpc_demo.ibmw.yaml -o dds_rpc_demo_gen

# 构建
cd dds_rpc_demo_gen && mkdir -p build && cd build
rm -rf CMakeCache.txt CMakeFiles
cmake .. -DCMAKE_PREFIX_PATH=<ibmw-install-dir>
make -j$(nproc)

# 设置运行时环境
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:<ibmw-install-dir>/lib:<ibmw-install-dir>:<ibmw-install-dir>/bin:$LD_LIBRARY_PATH"

# 运行（双终端）
./dds_rpc_server_app cfg/dds_rpc_server_app.yaml   # Terminal 1
./dds_rpc_client_app cfg/dds_rpc_client_app.yaml   # Terminal 2

# 验证 ROS2 能看到该 service（可选）
ros2 service list

# ROS2 互操作：从 ROS2 调用 IBMW server
ros2 service call /add_two_ints example_interfaces/srv/AddTwoInts "{a: 3, b: 5}"

# ROS2 互操作：IBMW client 调用 ROS2 server
ros2 run demo_nodes_cpp add_two_ints_server   # Terminal 3
# 然后运行 IBMW client —— 它会调用 ROS2 server
```

**所需 Extension：** DDS。

---

### 15. iceoryx_inproc_demo —— Iceoryx 单进程 Pub/Sub

**目的：** 在单进程中演示 Iceoryx 共享内存 Transport，覆盖从 1 KB 到 1 MB 的
消息触发自动 SHM 扩容。

**演示要点：**
- `IBMW_BUILD_ICEORYX_EXTENSION` 编译路径
- `iceoryx_extension` 与 `type: iceoryx` Transport Driver 同时加载
- IDL 类型经 `IBMW_DDS_TYPE_NAME` + `RegisterSendType<T>` 注册
- Driver-agnostic 字节透传（iceoryx Driver 复用 `dds_types` CDR 序列化，
  运行时不依赖 FastDDS）
- 当 payload 超出 `shm_init_size` 时 SHM 段自动倍增
- **必须运行 RouDi 守护进程**（`iox-roudi`），单进程也不例外

**运行方式：**
```bash
# 构建（test 可选）
source /opt/ros/humble/setup.bash
cmake -B build -DIBMW_BUILD_ICEORYX_EXTENSION=ON
cmake --build build -j$(nproc) --target iceoryx_inproc_app iox-roudi \
                                        ibmw_samples_cpp_iceoryx_inproc_demo_build_all

# 运行（单终端 —— start_*.sh 自动管理 RouDi）
cd build
bash start_iceoryx_inproc.sh
```

**所需 Extension：** Iceoryx（`IBMW_BUILD_ICEORYX_EXTENSION=ON`）。

> **提示：** YAML 中的 `shm_init_size` 故意设得很小（256 字节），让 1 MB
> payload 触发 `LoanShm` 倍增循环。Iceoryx 自身的 SHM 扩容日志位于 Debug
> 级别；1 MB 样本端到端成功送达即可证明该循环工作正常。

---

### 16. iceoryx_interproc_demo —— Iceoryx 双进程 Pub/Sub

**目的：** 与 `iceoryx_inproc_demo` 相同的 payload 组合，但 Publisher 与
Subscriber 运行在独立进程中，通过同一个 RouDi 守护进程通信。

**演示要点：**
- 两个 IBMW Runtime 使用 **不同的 `runtime_id`** 共享同一 RouDi
- Subscriber 必须在 Publisher 第一次发送 **之前** 完成 Listener 注册
  （iceoryx 默认 history depth 较小）
- 跨进程零拷贝：1 MB payload 端到端 RTT 仍保持 ~1.5 ms 量级 ——
  跨越进程边界的只有 SHM 偏移量

**运行方式：**
```bash
# 构建
source /opt/ros/humble/setup.bash
cmake -B build -DIBMW_BUILD_ICEORYX_EXTENSION=ON
cmake --build build -j$(nproc) \
    --target iceoryx_interproc_publisher_app iceoryx_interproc_subscriber_app \
             iox-roudi ibmw_samples_cpp_iceoryx_interproc_demo_build_all

# 通过辅助脚本运行（自动按 RouDi → sub → pub 顺序启动）
cd build
DURATION=15 bash start_iceoryx_interproc.sh

# 或手动三终端启动：
./iox-roudi                                                # Terminal 1
./iceoryx_interproc_subscriber_app cfg/subscriber.yaml     # Terminal 2（先启）
./iceoryx_interproc_publisher_app cfg/publisher.yaml       # Terminal 3
```

**所需 Extension：** Iceoryx（`IBMW_BUILD_ICEORYX_EXTENSION=ON`）。

---

### 17. multi_driver_demo —— 单模块挂载多个 Driver

**目的：** 演示变参宏 `IBMW_USE_DRIVERS(...)`，在**单个** Module 类内
同时绑定 **DDS + Iceoryx 两个 Driver** 的 typed slot，展示 typed
driver-slot ABI 可以横跨多个 Extension 组合使用。

**演示要点：**
- 一个 user-region 模块同时持有 `DdsTransportDriver*` 与
  `IceoryxTransportDriver*` 两个成员指针
- 通过单条 `IBMW_USE_DRIVERS(IBMW_DRIVER_ENTRY(...), IBMW_DRIVER_ENTRY(...))`
  声明同时绑定两个 Driver（单 driver 场景使用 `IBMW_USE_DRIVER`）
- 同时覆盖 4 种使用形态：managed 源码、standalone app 二进制、pkg `.so`、
  ibmw-gen 生成（regen 字节相等）

**运行方式：**
```bash
cd build

# App 模式（不依赖任何 transport extension 在线）
./multi_driver_demo_app ./cfg/multi_driver_demo_app_mode_cfg.yaml

# Pkg 模式
./bin/ibmw run ./cfg/multi_driver_demo_cfg.yaml
```

**所需 Extension：** DDS（`IBMW_BUILD_DDS_EXTENSION=ON`）+
Iceoryx（`IBMW_BUILD_ICEORYX_EXTENSION=ON`），由 `full` preset 默认启用。

**最小代码示例：**
```cpp
class MultiDriverModule : public ibmw::Module {
 private:
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* iox_drv_ = nullptr;

  // 单条 IBMW_USE_DRIVERS 把两个 driver 同时绑到本模块
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver,
                        dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxTransportDriver,
                        iox_drv_))
};
```

> 完整源码与代码走读详见 `src/samples/cpp/multi_driver_demo/README.md`。

---

## YAML 配置参考

所有 IBMW 配置位于 `ibmw:` 顶层 key 下。模块专属配置使用与 Module 名匹配的
独立顶层 key。

支持环境变量：`${MY_VAR}` 在解析前会被替换。

### 顶层结构

```yaml
ibmw:
  extensions: ...      # Extension/插件加载
  main_loop: ...       # 主线程 Executor
  watchdog: ...        # Watchdog 线程
  scheduling: ...      # 用户自定义 Executor
  logging: ...         # 日志后端与级别
  memory: ...          # Allocator 配置（保留）
  service: ...         # Service（RPC）Driver 与路由
  messaging: ...       # Transport Driver 与路由
  parameter: ...       # Parameter 存储（保留）
  modules: ...         # Package 加载、per-module 配置
  config: ...          # Config 子系统选项

MyModule: ...          # "MyModule" 的自定义配置
```

### 子系统初始化顺序

Extensions -> Main Loop -> Watchdog -> Executors -> Logging -> Memory ->
RPC -> Transport -> Parameters -> Modules -> Configurator

---

### `ibmw.extensions`

```yaml
ibmw:
  extensions:
    items:
      - name: <string>       # 唯一 Extension 名（必需）
        path: <string>       # .so 库路径（必需）
        options: { ... }     # Extension 专属配置（可选）
```

---

### `ibmw.main_loop`

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `name` | string | `"main_loop"` | Executor 名 |
| `thread_sched_policy` | string | `""` | OS 调度策略（如 `"SCHED_FIFO:50"`） |
| `thread_bind_cpu` | uint32[] | `[]` | CPU 亲和性列表 |

---

### `ibmw.watchdog`

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `name` | string | `"watchdog"` | Executor 名 |
| `thread_sched_policy` | string | `""` | OS 调度策略 |
| `thread_bind_cpu` | uint32[] | `[]` | CPU 亲和性列表 |
| `queue_threshold` | uint32 | `0` | 队列深度告警阈值（0=禁用） |
| `threshold_alarm_interval_ms` | int32 | `0` | 告警最小间隔（ms） |

---

### `ibmw.scheduling`

Executor 定义数组：

```yaml
ibmw:
  scheduling:
    - name: <string>      # 唯一名称（必需）
      type: <string>      # Executor 类型（必需）
      options: { ... }    # 类型专属选项
```

#### `thread_pool` 选项

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `thread_num` | uint32 | `1` | 线程数 |
| `thread_sched_policy` | string | `""` | OS 调度策略 |
| `thread_bind_cpu` | uint32[] | `[]` | CPU 亲和性 |
| `timeout_alarm_threshold_us` | uint64 | `0` | 任务超时告警（us，0=禁用） |
| `use_system_clock` | bool | `false` | 超时使用系统时钟 |

#### `dedicated` 选项

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `thread_sched_policy` | string | `""` | OS 调度策略 |
| `thread_bind_cpu` | uint32[] | `[]` | CPU 亲和性 |
| `queue_threshold` | uint32 | `0` | 队列深度告警（0=禁用） |

#### `strand` 选项

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `bind_thread_pool_executor_name` | string | -- | **必需。** 要绑定的线程池 |
| `timeout_alarm_threshold_us` | uint64 | `0` | 任务超时告警（us） |

#### `timer` 选项

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `bind_executor` | string | `""` | 要绑定的 Executor（空=独立线程） |
| `dt_us` | uint64 | `1000` | Tick 分辨率（us） |
| `wheel_size` | size_t[] | `[1000]` | 时间轮分级大小 |
| `thread_sched_policy` | string | `""` | OS 调度策略（仅独立线程） |
| `thread_bind_cpu` | uint32[] | `[]` | CPU 亲和性（仅独立线程） |

#### `parallel` 选项

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `thread_num` | uint32 | `1` | TBB 线程数 |
| `thread_sched_policy` | string | `""` | OS 调度策略 |
| `thread_bind_cpu` | uint32[] | `[]` | CPU 亲和性 |
| `queue_threshold` | uint32 | `0` | 队列深度告警（0=禁用） |

---

### `ibmw.logging`

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `core_level` | string | `"Info"` | 框架日志级别：Trace/Debug/Info/Warn/Error/Fatal/Off |
| `default_module_level` | string | `"Info"` | 默认 Module 日志级别 |
| `enable_crash_log` | bool | `true` | 启用 crash 日志的信号处理器 |
| `backends` | array | `[]` | 日志后端配置 |

#### Console 后端（`type: console`）

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `color` | bool | `true` | ANSI 彩色输出 |
| `module_filter` | string | `""` | Module 名正则过滤 |
| `pattern` | string | `""` | 自定义日志格式 |
| `log_executor_name` | string | `""` | 异步日志的 Executor |

#### Rotate file 后端（`type: rotate_file`）

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `path` | string | `""` | 日志文件目录 |
| `filename` | string | `""` | 基础文件名 |
| `max_file_size_m` | uint32 | `10` | 单文件最大大小（MB） |
| `max_file_num` | uint32 | `5` | 最大轮转文件数 |
| `module_filter` | string | `""` | 正则过滤 |
| `pattern` | string | `""` | 自定义格式 |
| `enable_sync` | bool | `false` | 周期性 flush |
| `sync_interval_ms` | uint32 | `1000` | flush 间隔（ms） |
| `sync_executor_name` | string | `""` | flush 任务 Executor |
| `suffix_with_timestamp` | bool | `false` | 文件名追加时间戳 |

---

### `ibmw.service`

```yaml
ibmw:
  service:
    drivers:
      - type: <string>       # local, dds, http, grpc
        options: { ... }
    clients:
      - func_name: <regex>   # 匹配 RPC 函数名
        enable_drivers: [...]
        enable_filters: [...]
    servers:
      - func_name: <regex>
        enable_drivers: [...]
        enable_filters: [...]
```

#### Local RPC Driver（`type: local`）

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `timeout_executor` | string | `""` | Client 超时处理 Executor |
| `timeout_handle_executor` | string | `""` | Server 超时处理 Executor |

#### DDS RPC Driver（`type: dds`）

DDS RPC Driver 通过 FastDDS 实现跨进程 RPC，无需任何 rcl/rclcpp 依赖。它使用
**原生 `.srv` 模式**：业务 request/response 类型由 FastDDS 直接序列化，
TypeSupport 由 gencode 提供（无信封包装）。这使得线协议与 ROS2 标准 service
100% 兼容。

当 `ros2_compatible` 为 true（从 DDS Extension 加载时的默认值）时，使用 ROS2
service 线协议规范（`rq/`/`rr/` Topic 前缀、`dds_::` 类型名中缀、URL 编码
名称），使该 service 可被标准 ROS2 节点（`ros2 service call`、`ros2 service
list`）发现并调用。

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `ros2_compatible` | bool | `true` | 使用 ROS2 命名规范的 DDS Topic |

**工作原理：** 每个 RPC 函数映射到一个 request topic
（`rq/<service_name>Request`）和一个 reply topic（`rr/<service_name>Reply`）。
DDS 类型名和 CDR payload 由 `.srv` 定义派生（如
`example_interfaces::srv::dds_::AddTwoInts_Request_`），与 ROS2 完全一致。
请求/响应关联使用 RTPS `WriteParams` / `SampleInfo.related_sample_identity`
机制 —— 这正是 `rmw_fastrtps` 内部所用的机制。

**路由示例：**

```yaml
ibmw:
  service:
    drivers:
      - type: dds
        options:
          ros2_compatible: true
      - type: local
    clients:
      - func_name: "dds:(.*)"     # 路由 dds: 前缀的函数到 DDS
        enable_drivers: [dds]
      - func_name: "(.*)"         # 其余保持进程内
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
      - topic: <regex>       # 匹配 Topic 名
        enable_drivers: [...]
        enable_filters: [...]
    subscribers:
      - topic: <regex>
        enable_drivers: [...]
        enable_filters: [...]
```

#### Local Transport Driver（`type: local`）

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `subscriber_use_inline_executor` | bool | `false` | 在 publisher 线程中运行回调 |
| `subscriber_executor` | string | `""` | 默认 subscriber 回调 Executor |

---

### `ibmw.modules`

```yaml
ibmw:
  modules:
    packages:
      - path: <string>                 # .so 路径（必需）
        enable_modules: [<string>]     # 白名单（可选）
        disable_modules: [<string>]    # 黑名单（可选）
    modules:
      - name: <string>                 # Module 名（必需）
        log_level: <string>            # 覆盖日志级别
        enable: <bool>                 # 启用/禁用 Module
        cfg_file_path: <string>        # Module 配置文件路径
```

---

### Extension 专属选项

#### DDS Extension

Extension 选项：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `domain_id` | uint32 | `0` | DDS Domain ID |
| `participant_name` | string | `""` | DDS Participant 名 |

DDS Transport Driver 选项（`messaging` 下的 `type: dds`）：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `ros2_compatible` | bool | `false` | ROS2 兼容的 Topic 命名 |
| `ros2_topic_prefix` | string | `"rt"` | ROS2 Topic 前缀 |
| `publishers[].topic` | string | -- | Topic 名 |
| `publishers[].qos.history` | string | `"keep_last"` | `keep_last` 或 `keep_all` |
| `publishers[].qos.depth` | int | `10` | 历史深度 |
| `publishers[].qos.reliability` | string | `"reliable"` | `reliable` 或 `best_effort` |
| `publishers[].qos.durability` | string | `"volatile"` | `volatile` 或 `transient_local` |
| `subscribers[].executor` | string | `""` | 回调 Executor |
| `zero_copy.mode` | string | `"auto"` | `auto`、`strict`、`compat` |

可靠 `keep_last` Publisher 默认使用 100 个额外 Writer 缓存槽，优先保障常见机器人
和 SLAM 负载的功能可靠性，普通用户无需配置。内存敏感的部署可将
`IBMW_DDS_RELIABLE_EXTRA_SAMPLES` 设为 `0` 到 `INT32_MAX` 的整数，或设为 `-1`
恢复 Fast DDS 默认值。非法值会回退到安全默认值 100。该设置不会增加逻辑历史深度，
也不会应用到 Reader、best-effort Writer 或 `keep_all` Writer。

#### gRPC Extension

Extension 选项：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `thread_num` | uint32 | `1` | gRPC 线程数 |
| `listen_ip` | string | `"0.0.0.0"` | 监听地址 |
| `listen_port` | uint16 | `0` | 监听端口 |

gRPC RPC Driver 选项（`type: grpc`）：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `clients[].func_name` | string | -- | RPC 函数名 |
| `clients[].server_url` | string | -- | 远端 server URL |
| `servers[].func_name` | string | -- | RPC 函数名 |

#### Net（HTTP/TCP/UDP）Extension

Extension 选项：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `thread_num` | uint32 | -- | 网络 I/O 线程数（必需） |
| `http_options.listen_ip` | string | `"0.0.0.0"` | HTTP 监听 IP |
| `http_options.listen_port` | uint16 | `0` | HTTP 监听端口 |
| `tcp_options.listen_ip` | string | `"0.0.0.0"` | TCP 监听 IP |
| `tcp_options.listen_port` | uint16 | `0` | TCP 监听端口 |
| `udp_options.listen_ip` | string | `"0.0.0.0"` | UDP 监听 IP |
| `udp_options.listen_port` | uint16 | `0` | UDP 监听端口 |
| `udp_options.max_pkg_size` | uint32 | `65535` | 最大 UDP 数据报 |

#### MQTT Extension

Extension 选项：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `broker_addr` | string | -- | Broker 地址（必需） |
| `client_id` | string | -- | Client ID（必需） |
| `max_pkg_size_k` | uint32 | `0` | 最大包大小 KB |
| `reconnect_interval_ms` | uint32 | `5000` | 重连间隔 |
| `truststore` | string | `""` | CA 证书路径（TLS） |
| `client_cert` | string | `""` | Client 证书（TLS） |
| `client_key` | string | `""` | Client 密钥（TLS） |
| `callback_executor_name` | string | `""` | 回调 Executor |

MQTT Transport Driver（`type: mqtt`）的 per-topic QoS：0、1、或 2。

#### Iceoryx Extension

共享内存零拷贝 Transport。在任何使用 iceoryx Driver 的 IBMW 进程启动前，
必须先运行 `iox-roudi` 守护进程。

Extension 选项（`name: iceoryx_extension`）：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `shm_init_size` | uint64 | `1024` | 初次 loan 的 SHM 大小（字节）。当 payload 装不下时由 `LoanShm`/`UpdateLoanShm` 自动倍增。 |
| `runtime_id` | string | `"iceoryx<pid>"` | Iceoryx PoshRuntime 标识。共享同一 RouDi 的多个进程之间必须唯一。 |

Iceoryx Transport Driver 选项（`messaging` 下 `type: iceoryx`）：

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `listener_thread_name` | string | `""` | Subscriber Listener 线程名（`ps -L`/`htop` 可见）。 |
| `listener_thread_sched_policy` | string | `""` | OS 调度策略，例如 `"SCHED_FIFO:50"`。 |
| `listener_thread_bind_cpu` | uint32[] | `[]` | Listener 线程的 CPU 亲和性列表。 |

Topic 到 iceoryx URL 的映射：每个注册的 IBMW Topic 都被映射为三段
iceoryx `ServiceDescription`：
`/transport / <UrlEncode(topic_name)> / <UrlEncode(msg_type)>`。
当任一段超过 `iox::MAX_RUNTIME_NAME_LENGTH` 时，会以 `...` 省略号居中
截断（同时打印一条 Warn 日志）。

最小示例（单进程）：

```yaml
ibmw:
  extensions:
    items:
      - name: iceoryx_extension
        path: ./lib/libibmw_iceoryx_transport.so
        options:
          shm_init_size: 256        # 故意设小，触发自动扩容
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

跨进程 Pub/Sub 时，每个进程使用 **不同的** `runtime_id`（但都连接同一
RouDi），并且 Subscriber 必须在 Publisher 第一次发送之前启动。

#### ROS2 Extension

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `node_name` | string | -- | ROS2 节点名（必需） |
| `executor_type` | string | `"SingleThreaded"` | ROS2 Executor 类型 |
| `executor_thread_num` | uint32 | `1` | 线程数（MultiThreaded） |

#### Record/Playback Extension

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `service_name` | string | `""` | RPC service 名 |
| `timer_executor` | string | `""` | Timer Executor |
| `type_support_pkgs[].path` | string | -- | Type support .so |
| `record_actions[].name` | string | -- | Action 名 |
| `record_actions[].options.bag_path` | string | -- | Bag 存储目录 |
| `record_actions[].options.mode` | string | -- | `imd` 或 `signal` |
| `record_actions[].options.topic_meta_list[].topic` | string | -- | Topic |
| `record_actions[].options.topic_meta_list[].msg_type` | string | -- | 类型 |

---

### 完整示例

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

## 故障排查

### DDS Demo 报共享库错误

确保 ROS2 库在 library path 中：
```bash
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:<ibmw-build-dir>/lib:<ibmw-build-dir>:<ibmw-build-dir>/bin:$LD_LIBRARY_PATH"
```

### 构建失败提示 "FastCDR not found"

安装 FastCDR 或确保 `_deps/fastcdr-install/` 存在。在 ROS2 Humble 上：
```bash
sudo apt install ros-humble-fastrtps
```

### DDS Demo 关闭时挂起

部分 DDS Demo 关闭需要 30-60 秒（DDS 发现清理）。用 `timeout -s KILL <秒数>`
强制终止：
```bash
timeout -s KILL 10 ./dds_native_app ./dds_native_cfg.yaml
```

### Module .so 找不到

确认 YAML 配置中的路径相对于工作目录正确。为提高可移植性，使用绝对路径或
`${ENV_VAR}` 替换。

### 实时调度权限被拒

实时 Executor Demo（`SCHED_FIFO`、`SCHED_RR`）需要提升权限：
```bash
sudo ./bin/ibmw run ./cfg/examples_cpp_executor_real_time_cfg.yaml
```

或设置 capability：
```bash
sudo setcap cap_sys_nice+ep ./bin/ibmw
```

### ibmw-gen 项目 cmake 报错 "Could not find a configuration file for package ibmw"

`cmake --install . --prefix .` 之后，CMake 会在 build 目录生成
`ibmw-config.cmake` 与 `ibmw-config-version.cmake`。这些过期文件会干扰使用
`-DCMAKE_PREFIX_PATH=<build-dir>` 的 ibmw-gen 项目，导致 CMake 找到不完整的
配置而不是正确的 target。

**修复：** 在 install 后删除这些过期文件：
```bash
cd build
rm -f ibmw-config.cmake ibmw-config-version.cmake
```

上述标准构建说明中已包含此步骤。
