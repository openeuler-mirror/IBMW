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

### 查询 Service

`ibmw service list`、`type` 和 `find` 支持 registry、DDS、ROS 2、Zenoh、MQTT、HTTP
和 gRPC 发现。默认使用 registry，读取本机同一用户的有效 IBMW 运行时登记信息。
网络来源使用独立的发现观察器，无需启动 Engine。这些命令只查询元数据，不调用业务 RPC。

```bash
ibmw service list
ibmw service list --registry -t
ibmw service list --count-services
ibmw service type 'json:/my_project.ComputeService/Compute'
ibmw service find 'json:my_project.ComputeRequest -> json:my_project.ComputeResponse'
ibmw service find 'json:my_project.ComputeRequest -> json:my_project.ComputeResponse' -c
ibmw service list --include-hidden-services
# 先将 IBMW_REGISTRY_PATH 设为运行时使用的私有注册根目录（当前用户所有，权限 0700）。
ibmw service list --registry-path "$IBMW_REGISTRY_PATH"
```

对于 registry、HTTP、gRPC、MQTT 和 Zenoh，服务名是**完整的已注册 RPC 方法名**，
包含类型前缀；`type` 输出准确的 `request_type -> response_type` 类型对。
ROS 2 返回原生服务名和 `pkg/srv/Type` 类型。DDS 返回线协议服务名和请求/响应类型对，
识别 ROS 2 的 `rq/...Request`/`rr/...Reply` 和 IBMW 的
`ibmw_rpc/rq/...`/`ibmw_rpc/rr/...` 约定；不会还原 IBMW 逻辑名称，
也不支持发现任意厂商的 RPC 协议。DDS 服务端必须在同一个 participant 中同时具有
请求 reader 和响应 writer。

`find` 精确匹配 `type` 返回的完整字符串。类型对需像示例中一样加引号，避免 Shell 将
`>` 解释为重定向。名称和类型会排序、去重；同一名称对应多个类型时，`type` 每行输出一个，
`list -t` 列出全部类型。`list -c` 和 `find -c` 统计匹配的唯一服务名数量，而非端点数量。

`list -t` 输出示例：

```text
json:/my_project.ComputeService/Compute [json:my_project.ComputeRequest -> json:my_project.ComputeResponse]
```

这些命令只显示**服务端注册**，仅注册客户端不会新增可列出的服务。服务名按 `/` 分隔后，
任一名称分量以 `_` 开头即视为隐藏服务；`--include-hidden-services` 可以放在子命令之前
或之后。注册信息只表示元数据，不代表服务已就绪或通信已成功。

对于 registry 发现，运行时在初始化期间采集 ServiceCatalog 元数据，并在 Service 子系统
启动前刷新，包括 App 模式下在 `Engine::Initialize()` 与 `Start()` 之间新增的注册。
登记信息包含服务端/客户端角色、完整方法名、请求/响应类型、模块/包标识，以及已启用的
RPC Driver。本机来源可以表示所有 RPC Driver，并不限于 `local`。
运行时需启用 `ibmw.runtime_graph.enabled`（默认开启）才能发布登记信息。

`--registry-path` 指定注册根目录，与 `ibmw.runtime_graph.registry_path` 一致，
命令自动使用其 `v2/` 子目录。默认根目录为 `$XDG_RUNTIME_DIR/ibmw`，未设置时为
`/tmp/ibmw-<euid>`。查询成功但结果为空时退出码为 0；`type` 查询不存在或被隐藏的服务时
退出码为 1，发现错误也返回 1。旧版本运行时缺少服务元数据时，命令提示升级并重启，
保留新版本运行时的可用结果并返回 1。需要使用更新后的运行时重新构建/部署应用并重启，
才能发布服务元数据。已有 Topic 和 Module 登记仍可读取；已退出运行时的记录由现有
registry 存活检查机制处理。

#### 网络来源

```bash
ibmw service list --dds --dds-domain 10 -t
ibmw service list --ros2 --ros-domain 10 -t
ibmw service type /add_two_ints --ros2
ibmw service find example_interfaces/srv/AddTwoInts --ros2
ibmw service list --zenoh --zenoh-domain robot --zenoh-config ./zenoh.json5
ibmw service list --mqtt tcp://192.168.1.10:1883 -t
ibmw service list --http 192.168.1.10:50080 -t
ibmw service list --grpc 192.168.1.10:50051 -t
ibmw service list --registry --dds --ros2 --domain 10 --timeout 3
```

来源选项适用于三个子命令，可以组合使用。短选项为 `-d`（DDS）、`-r`（ROS 2）和
`-z`（Zenoh）。`--domain` 作用于已选择的 DDS/ROS 2 来源，各来源专用的 domain 选项
优先于它。DDS 默认使用 domain 0；ROS 2 未指定 domain 时使用 `ROS_DOMAIN_ID`，
再默认到 0。Zenoh 使用原生配置和 `limit_domain`（默认为空）。网络发现插件随对应的
Extension 构建选项启用；缺少插件时命令报错。

HTTP 和 gRPC 必须显式指定**服务端**地址，CLI 主动连接已有监听端口。
HTTP 使用只读端点 `GET /ibmw/discovery/services/v1`；gRPC 使用 IBMW 专用的
`/ibmw.discovery.v1.ServiceDirectory/List` 一元方法，通过 gRPC 帧传输 JSON，
不使用 gRPC reflection。这两种来源目前使用明文 HTTP/HTTP2。
MQTT 必须显式指定 broker URI（`tcp://host:port`，或使用受信任服务端证书的
`ssl://host:port`）；CLI 暂未提供认证凭据和自定义客户端证书选项。
每次 MQTT 查询先订阅独立的临时回复主题，再发布目录请求；回复不设置 retained 标志。
Zenoh 查询运行时持有的目录 queryable。发现服务端无需预先知道 CLI 的 IP，后续启动的
CLI 进程也可独立查询。

HTTP、gRPC、MQTT 和 Zenoh 的目录响应器需要更新后的 IBMW 运行时，并配置对应的
RPC Driver。它们只公开成功注册的服务端名称和类型，包括 App 模式下 Start 之前的注册，
不会导出 registry 路径或模块库路径。DDS 和 ROS 2 使用原生发现，也可发现符合相应协议的
原生服务端。MQTT broker ACL 必须允许访问 `ibmw/discovery/services/v1/request`
和 `ibmw/discovery/services/v1/reply/+` 主题。目录访问沿用现有监听端口、broker
或会话的访问控制。

MQTT 的 `list/type/find` 支持 `--mqtt ssl://host:port`。使用私有 CA 时提供
`--mqtt-truststore <path>`，否则使用系统信任库；双向 TLS 需要同时提供
`--mqtt-client-cert <path>` 和 `--mqtt-client-key <path>`。证书配置仅适用于
TLS MQTT 来源，认证或连接失败返回退出码 1，不作为成功的空目录。

`--timeout` 设置共享采集窗口，默认 2 秒，范围为 0.01–60 秒；选中的来源并发查询。
原生会话建立和 transport 清理可能带来额外耗时。成功返回空结果不代表所有对端均可达，
发现较慢的网络应适当延长窗口。任一来源不可用或出错时，命令输出诊断并返回退出码 1，
同时保留其他来源的有效结果。旧版 HTTP/gRPC 服务端缺少目录方法时会报错；
旧版 MQTT/Zenoh 运行时则不会回复元数据查询。

### 调用 Service

`ibmw service call <service> <type> '<request>' [options]` 创建临时 RPC 客户端，
发送一次请求，输出 JSON 响应后退出。请求接受单个 JSON/YAML 对象。
类型加载、文本转换和调用生命周期由 service 命令独立实现。

```bash
# HTTP：使用已安装的请求/响应 TypeSupport 包
ibmw service call 'json:/my_project.ComputeService/Compute' \
  'json:my_project.ComputeRequest -> json:my_project.ComputeResponse' \
  '{value: 21}' --http 127.0.0.1:50080 \
  --type-support ./libmy_project_typesupport.so --timeout 5

# DDS：必须使用类型包中的规范类型名，不能直接把发现得到的 wire type 当作它
ibmw service call 'dds:/my_project.EchoService/Echo' \
  'dds:my_project::Request -> dds:my_project::Response' \
  '{value: 21}' --dds --domain 10 --type-support ./libmy_project_typesupport.so

# 原生 ROS 2：从已 source 的 ROS 安装中加载服务和消息 introspection
ibmw service call /add_two_ints example_interfaces/srv/AddTwoInts \
  '{a: 19, b: 23}' --ros2 --ros-domain 10
```

每次调用必须且只能选择一个后端：`--dds`、`--ros2`、`--zenoh`、
`--mqtt <broker-uri>`、`--http <host:port>` 或 `--grpc <host:port>`。
HTTP 和 gRPC 地址也可使用 `http://host:port`，当前不提供 HTTPS。
gRPC 方法名支持 `cdr:`、`dds:`、`ros2:` 或 `json:` 前缀。
使用 `--transport-library <path>` 可显式指定所选后端的运行时库。
显式方法名允许调用隐藏服务；`--include-hidden-services` 也被接受。

除原生 ROS 2 外，`<type>` 必须是带引号的 `request_type -> response_type`；
`--type-support <library>` 可以重复，用来加载两种类型。两种 TypeSupport 都必须
支持 JSON 文本转换；发送前先验证请求。DDS 还要求原生 DDS TypeSupport 和 CDR 支持。
DDS 默认使用 IBMW RPC topic 命名；调用 ROS 2 兼容服务时增加 `--ros2-compatible`，
服务名使用实际线协议名称，请求/响应类型必须与对端 DDS 类型和布局兼容。
这条 DDS 路径不依赖 rcl，也不自动从 `.srv` 名称生成类型支持。
原生 `--ros2` 则要求构建 `IBMW_BUILD_ROS2_EXTENSION=ON`，部署
`libibmw_service_call_ros2.so`，并安装相应的 C++ service typesupport 和
introspection 库；此模式不使用 `--type-support`。

`--domain`、`--dds-domain`、`--ros-domain`、`--zenoh-domain` 和
`--zenoh-config` 沿用对应后端的连接配置。MQTT 可设置 `--mqtt-client-id`、
`--mqtt-server-id`、`--mqtt-qos`；TLS broker 使用 `ssl://` 或 `mqtts://`，
必须提供 `--mqtt-truststore`，可配套提供 `--mqtt-client-cert` 和
`--mqtt-client-key`。HTTP/gRPC/MQTT/Zenoh 可通过 `--serialization` 选择两种
类型共同支持的格式，否则优先使用请求类型的默认格式。

`--discovery-timeout` 是客户端就绪等待上限（默认 2 秒），`--timeout` 是发送后
的响应等待上限（默认 5 秒），两者互相独立；初始化和资源清理另有开销。
DDS 等待请求与响应端点匹配，原生 ROS 2 查询客户端可用状态，MQTT 等待连接和
订阅确认，Zenoh 等待请求订阅端匹配。连接就绪不保证业务调用一定成功。命令不自动重发请求；响应超时或
Ctrl+C 后，服务端仍可能已执行请求。

成功返回 0，stdout 只有 JSON 响应；失败返回 1，诊断写入 stderr。
错误状态沿用所选 Driver 的映射；HTTP Driver 对非 200 响应不保留原始服务端状态码。
SIGINT/SIGTERM 分别返回 130/143。registry 只是发现来源，`local` 只在进程内通信，
本命令不通过 registry 控制 socket 转发其他进程的纯 local 服务。需要显式选择
服务端已经启用的网络 Driver 及其地址/domain；不会自动选择一个同名服务实例。

### 查看和修改运行时参数

`ibmw param list [module]`、`ibmw param get <module> <parameter_name>` 和
`ibmw param set <module> <parameter_name> <value>` 通过集中式参数服务访问
IBMW 字符串参数，连接选项沿用 `service call`。`set` 只修改运行时值。
服务端配置、多实例命名和完整示例见 [参数命令](parameters.zh.md)。

### 查看和修改运行时日志级别

`ibmw log list [module]` 列出所选日志服务中的 logger 名称及当前级别，
`ibmw log set <module> <level>` 修改已有 logger 的级别；`core` 表示框架日志。
连接选项沿用 `service call`，不依赖 Runtime Agent，也不修改启动配置。
服务端配置、后端选择及示例见 [日志命令](log-control.zh.md)。

### 查看本机模块

`ibmw module list` 通过 RuntimeRegistry 列出本机同一用户的有效运行时中登记的模块，
每行输出 `runtime_id/module_name [pid: <PID>]`，默认显示所属进程的 PID，
按运行时 ID 和模块名排序。同名模块在不同运行时中分别显示。

```bash
ibmw module list
ibmw module list --runtime robot-main
ibmw module list --count-modules
# 先将 IBMW_REGISTRY_PATH 设为运行时使用的私有注册根目录（当前用户所有，权限 0700）。
ibmw module list --registry-path "$IBMW_REGISTRY_PATH"
```

`--count-modules`（`-c`）只输出模块实例数量。
`--registry-path` 指定注册根目录，与 `ibmw.runtime_graph.registry_path` 一致，命令自动使用其 `v2/` 子目录。
默认根目录为 `$XDG_RUNTIME_DIR/ibmw`，未设置时为 `/tmp/ibmw-<euid>`。

列表来自 ModuleBus，包含包加载、直接注册的模块及 App 模式 `CreateModule()` 创建的运行上下文，
即使模块没有任何话题也能列出。只有通信端点上的 `module_name` 不会产生模块记录。
模块列表在初始化时写入，并在模块启动前刷新；它表示已登记模块，不代表业务健康状态。
运行时需启用 `ibmw.runtime_graph.enabled`（默认开启），关闭注册功能的进程不会出现在结果中。

没有模块时输出为空、退出码为 0；指定的运行时不存在时退出码为 1。
旧版本注册文件缺少模块列表时，命令提示升级并重启对应运行时，保留其他可用结果并返回 1；
计数模式此时只统计提供模块信息的运行时。进程退出后的过期记录由现有 RuntimeRegistry 机制处理。

### 模块生命周期记录（基础接口）

ModuleBus 在准备模块访问接口及执行 `Setup()`、`Run()` 时记录每个模块的结果。
进程内通过 `GetModuleLifecycleStatuses()` 获取按模块名排序的线程安全值快照；
Runtime Agent 提供只读 `ListModuleLifecycle` 请求，C++ 客户端对应
`RuntimeGraphAgentClient::ListModuleLifecycle()`。查询不会调用模块回调或修改登记文件。
`ibmw doctor module` 已使用本接口检查初始化和启动结果，根命令 `ibmw doctor`
也包含此检查；`module list/info` 保持原有行为。用法及查询范围见
[Doctor 模块生命周期检查](doctor.zh.md#模块生命周期检查)。

每条记录包含 `module_name`、`package_path`、`kind`、`initialize`、`start`、
`failure` 和 `updated_at_unix_ms`。阶段结果分别为 `not_attempted`（尚未执行）、
`in_progress`（执行中）、`succeeded`（成功）、`failed`（失败）或
`not_applicable`（不适用）；失败信息包含 `phase` 和 `reason`。
时间戳为最近一次记录状态变化的 Unix 毫秒时间，不是心跳。

`kind=managed` 包括包加载和直接注册的模块；初始化涵盖访问接口配置及 `Setup()`，
启动对应 `Run()`。`kind=app_context` 表示 `CreateModule()` 创建的运行上下文：
初始化结果只表示访问接口配置结果，`start` 始终为 `not_applicable`，不会虚构启动成功。
失败返回值、传播到 ModuleBus 的标准或非标准异常都会记录；原有失败传播行为保持不变。
C++ 模块包装层已捕获并转成 `false` 的异常只能记录回调失败，详细原因仍以原日志为准。

记录在进入单个模块初始化时建立。禁用的模块，以及在此之前发生的包加载、模块创建、
元信息读取等失败不在本接口覆盖范围内；初始化中断后尚未处理的模块也不会生成记录。
记录独立于模块对象保存，关闭后仍能通过进程内接口查询，但只保留到 ModuleBus 销毁。
同名 App 上下文初始化失败后重试时，以本次尝试覆盖上次记录。
阶段成功仅表示该次框架操作成功，不表示模块当前仍在运行或业务健康。

Runtime Agent 必须启用并已开始服务才能从其他进程查询。当前 Engine 的初始化、启动
顺序意味着模块失败可能早于 Agent 开始服务，此时只能由宿主通过进程内接口读取记录。
本版本不提供失败落盘、进程退出后查询、运行中健康上报或卡死检测。
旧 Agent 或未设置生命周期查询能力的 Agent 返回不支持错误，不会伪装为空模块列表。

### 查看模块通信接口

`ibmw module info <module>` 显示已登记模块的名称、运行时 ID、PID，以及发布和订阅的话题与消息类型。
参数为模块名；同名模块存在于多个运行时时，命令列出候选运行时并返回 1，使用 `--runtime` 指定目标。
`--registry-path` 与 `module list` 使用相同的注册根目录规则。

```bash
ibmw module info perception
ibmw module info perception --runtime robot-main
ibmw module info perception --registry-path "$IBMW_REGISTRY_PATH"
```

示例输出：

```text
Module: perception
Runtime: robot-main
PID: 12345

Publishers:
  /detections [json:DetectionArray]

Subscribers:
  /camera/image [json:Image]
```

各方向按话题名和消息类型排序，相同话题和类型的重复端点只显示一次，包含注册表中标记为隐藏的话题。
没有对应接口时显示 `(none)`，无话题模块也可成功查询。结果来自运行时登记的接口快照，
不代表当前消息流量、连接状态或业务健康；未登记的端点不会显示。
模块或指定运行时不存在时返回 1。旧运行时缺少模块列表时提示升级重启，保留唯一可用匹配的输出并返回 1；
只出现在话题端点上的模块名不会被视为已登记模块。可用信息完整且唯一匹配时返回 0。

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
| `IBMW_BUILD_SHM_EXTENSION` | 自研共享内存 Transport（零拷贝，无 RouDi，不依赖 iceoryx；仅 Linux/OHOS）。可与 iceoryx 同时开启；预设 `shm` |
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
- `ibmw::executor::CreateTimer(executor, interval, callback)` —— 周期 Timer（须保存返回的句柄，句柄释放后定时器停止）
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
      - type: <string>       # local, dds, mqtt, http, tcp, udp, iceoryx, shm
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

#### Shm Extension（自研共享内存）

零拷贝共享内存 Transport，不需要 RouDi，也不链接 iceoryx（构建开关
`IBMW_BUILD_SHM_EXTENSION`，仅 Linux 内核，含 OHOS）。兼容 iceoryx_ext 的 API
（`shm_ext/iox_style_publisher.h`），但两者**不互通**：同一 Topic 的所有进程要用同一种。

```yaml
ibmw:
  extensions:
    items:
      - name: shm_extension
        path: ./libibmw_shm_transport.so
        options:
          runtime_id: my_shm_app
          # driver_name: iceoryx  # 原位替换 iceoryx_extension，messaging 段保持 type: iceoryx
  messaging:
    drivers:
      - type: shm
    publishers:
      - topic: "(.*)"
        enable_drivers: [shm]
    subscribers:
      - topic: "(.*)"
        enable_drivers: [shm]
```

`driver_name` 缺省为 `shm`。核心只使用第一个同名驱动，所以与 `iceoryx_extension`
同时加载时不能设 `driver_name: iceoryx`，否则启动失败。OHOS 上必须设置 `root_dir`
（或环境变量 `IBMW_SHM_ROOT`）。`driver_name: iceoryx` 只对按名查找驱动的代码透明：驱动槽位恒为
`shm`，按 iceoryx 驱动类型绑定的模块（`IBMW_USE_DRIVER`、生成的 `direct_iceoryx_*` 模块）要改用 shm
头文件或重新生成。全部选项、行为差异与未替代项见
[自研共享内存使用与迁移](../../selfdev/shm/docs/使用与迁移.md)。

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
| `record_actions[].name` | string | -- | Action 名 |
| `record_actions[].options.bag_path` | string | -- | Bag 存储目录 |
| `record_actions[].options.mode` | string | -- | `imd` 或 `signal` |
| `record_actions[].options.topic_meta_list[].topic` | string | -- | Topic |
| `record_actions[].options.topic_meta_list[].msg_type` | string | -- | 类型 |

Runtime Graph（`ibmw.runtime_graph`）统一负责运行时广告、话题发现和控制
Socket。需要用于话题 schema、录制或回放的 TypeSupport 包配置在
`ibmw.runtime_graph.type_support_pkgs` 下；Record/Playback 扩展本身不再
单独维护这份配置。

#### 话题 CLI

**性能观察结论**

以下结论基于 i7-12700、Linux 5.15、ROS 2 Humble / Fast DDS 2.6.12
环境下的同机跨进程重复测试；观测负载为 64KiB / 1MiB、1 / 4 个发布者和观察进程。

- **原生业务观测**：开启 `topic hz/bw/echo` 会增加资源开销和部分时延；本次四个场景中，IBMW 的发送到接收 p99 更低，ROS 2 的发布调用 p99 更低，两侧消息类型、观察 QoS 和 writer 数不同。
- **同一 ROS 业务观测**：七个场景中，IBMW 观察 CLI 的 CPU 均低于 ROS 2 CLI，PSS 内存均更高，多观察者下业务时延的增加更明显。
- **端点查询**：相同 2 秒发现窗口下，IBMW `topic list/info/type/find` 的命令耗时和累计 CPU 时间均较低；此结论仅描述查询命令自身开销。
- **CLI 主动发布**：64KiB、总 10Hz、1 / 4 个 CLI 的三轮测试均收全；IBMW `topic pub` 的累计 CPU 时间和 PSS 较低，生命周期耗时较长，其中包含本次测试设置的 3 秒就绪等待，ROS 2 则等待订阅匹配。

`ibmw topic pub <topic> <type> <values>` 从命令行创建发布端，无需运行配置文件，
也不生成临时运行配置。后端、domain、QoS 等参数直接构造内存配置，复用 Engine
的初始化、启动、关闭以及现有 TransportBus 发布流程。无需先启动业务模块或已有发布者。

```bash
# 使用 common 类型包，一次发送；类型包路径按安装位置调整
ibmw topic pub /header dds:ibmw::protocols::common::Header \
  '{"frame_id":"base","time_stamp":123,"sequence_num":1}' \
  --dds --domain 0 --type-support ./libcommon_dds_ts.so --once

# 每秒 5 条，共发送 10 条；退出前继续存活 0.5 秒
ibmw topic pub /header dds:ibmw::protocols::common::Header \
  '{frame_id: "base"}' --zenoh --zenoh-domain robot \
  --type-support ./libcommon_dds_ts.so --rate 5 --times 10 --keep-alive 0.5
```

必须选择一个传输后端：`--dds`、`--ros2`、`--zenoh`、`--mqtt`、`--tcp`、
`--udp`、`--http`、`--iceoryx`、`--shm` 或 `--local`。类型库通过可重复的
`--type-support` 提供；可用 `--transport-library` 覆盖默认驱动库路径。
`--domain`、`--dds-domain`、`--ros-domain`、`--zenoh-domain` 和
`--zenoh-config` 沿用观察命令的连接参数含义。`--ros2-compatible` 仅用于 DDS，
开启现有驱动的 ROS 2 兼容话题命名，供 `ros2:` 消息与 ROS 2 订阅者互通。
未启用时保留 IBMW DDS 默认命名；DDS 网络查询显示 wire topic，Registry 查询显示输入的逻辑 topic。

| 后端 | 连接参数 |
|------|----------|
| DDS / ROS 2 | `--domain` 或对应的 `--dds-domain` / `--ros-domain` |
| Zenoh | `--zenoh-domain`，可选 `--zenoh-config` |
| MQTT | 必须提供 `--broker tcp://host:port`；可选 `--client-id`、`--mqtt-qos 0\|1\|2`（默认 2） |
| TCP / UDP | 必须提供 `--target IPv4:port`，可重复指定多个接收端 |
| HTTP | 必须提供 `--target http://host:port`，可重复指定多个接收端 |
| Iceoryx | 先启动 RouDi；可选 `--iceoryx-runtime-id`、`--shm-init-size`（默认 1024 字节） |
| Shm（自研） | 无需 RouDi；可选 `--shm-domain`、`--shm-root-dir`、`--shm-runtime-id`、`--shm-group`、`--shm-init-size` 等 `--shm-*` 选项 |
| local | 仅当前 CLI 进程内发送，无法投递给另一进程的 local 订阅者 |

MQTT TLS 连接使用 `ssl://host:port`，必须提供 `--mqtt-truststore`；
可提供 `--mqtt-client-cert`、`--mqtt-client-key` 和 `--mqtt-key-password`。
TCP/UDP/HTTP 可用 `--net-threads` 设置网络线程数（默认 2）。网络后端使用现有 IBMW
传输协议；HTTP 请求路径由 topic/type 生成，`--target` 不接受自定义路径或 HTTPS。
除 local 外，所选驱动扩展必须已编译并安装；TCP/UDP/HTTP 共用 net 扩展。

```bash
ibmw topic pub /header dds:ibmw::protocols::common::Header '{frame_id: "base"}' \
  --mqtt --broker tcp://127.0.0.1:1883 --type-support ./libcommon_dds_ts.so --once
ibmw topic pub /header dds:ibmw::protocols::common::Header '{frame_id: "base"}' \
  --tcp --target 127.0.0.1:5000 --type-support ./libcommon_dds_ts.so --once
ibmw topic pub /header dds:ibmw::protocols::common::Header '{frame_id: "base"}' \
  --iceoryx --type-support ./libcommon_dds_ts.so --once
```

发布后端与发现来源独立：`DiscoveryBackend` 表示 list/info 等命令从哪里查询端点
（registry、DDS、ROS 2、Zenoh），不限制 pub 可用的传输驱动。pub 不要求先发现话题。

默认以 1 Hz 重复发布同一条消息，直到 Ctrl+C；`--once` 和 `--times` 互斥。
`--rate` 没有 `-r` 别名，`-r` 保留给 ROS 2 后端。
`--wait` 是启动后的连接/发现延时（默认 1 秒），`--keep-alive` 是最后一次发送后的存活时间
（默认 0.2 秒）。两者均不保证订阅端已匹配或远端已确认；输出计数表示调用发送接口的次数。
DDS/ROS 2 支持 `--qos-reliability`、`--qos-durability`、`--qos-history`、
`--qos-depth`，默认分别为 reliable、volatile、keep_last、10。

消息内容可以是 JSON 或 YAML，必须作为一个命令行参数传入。标准 TypeSupport
直接提供 JSON 转换，类型包随源码重新构建，不需要单独选择文本适配器：

- DDS：`GetDdsMessageTypeSupport<T>()` 同时支持 CDR 和 JSON。IDL 生成的类型头文件
  自动包含对应 `*Json.hpp`；手写 DDS 类型需提供 `IbmwFromJson` / `IbmwToJson` 字段转换。
- ROS 2：`ibmw::GetRos2MessageTypeSupport<T>()` 同时支持 ROS 2 序列化和 JSON，
  类型包链接消息包的 introspection C++ 库。
- JSON 头文件依赖由公共 TypeSupport CMake target 传递。

文本转换支持基础数值、布尔、字符串、嵌套消息和数组/序列；DDS 还支持生成的枚举。
未提供字段保留消息默认值，未知字段、类型错误、数值越界和数组长度错误会被拒绝。
当前不支持的字段种类（例如 union、map、宽字符串）会报错，不会静默忽略用户输入。
DDS bounded string 仍受所选 FastCDR/代码生成器兼容性限制。

`ibmw topic echo <topic>` 将收到的消息输出为 YAML，以 `---` 分隔。
`--format json` 改为每行一个 JSON 值；`--once` 输出一条后退出；
`--field header.stamp` 选择嵌套字段，数字路径段表示数组下标，例如
`--field poses.0.position`。发现和订阅路径与 `hz`、`bw` 共用，支持相同的
发现来源、类型、连接配置、QoS、超时和观察时长参数，不提供统计窗口参数。
`--queue-size <条数>` 设置待打印队列上限，必须为正整数，默认 100 条。
队列满时会报错退出；增大上限可以缓冲短时积压，但会增加内存占用，
无法解决接收速度持续超过输出速度的问题。

```bash
ibmw topic echo /sensor_data --once
ibmw topic echo /status --format json --field state
ibmw topic echo /chatter --ros2 --type-support ./libtext_ts.so --once
ibmw topic echo /binary_topic --raw --duration 10
ibmw topic echo /sensor_data --queue-size 1000
```

JSON/YAML 负载可直接显示。二进制负载需要匹配的 IBMW TypeSupport 包，同时支持
接收格式的反序列化及 JSON 或 YAML 序列化。Registry 模式可使用所选发布者登记的
类型包；显式传入的 `--type-support <path>` 优先，且该选项可重复。网络模式只加载
此选项或 `--config-file` 中 `ibmw.runtime_graph.type_support_pkgs` 指定的本机包，
配置中的相对路径以配置文件目录为基准，不从网络发现信息加载类型包。
标准 DDS/ROS 2 TypeSupport 同时提供结构化 echo 所需的文本转换。
解码能力不足时明确报错；`--raw` 可直接输出十六进制负载，无需解码，
不能与 `--field` 或 `--format` 同用。

DDS 模式查看 ROS 消息时，先查找与 `dds:<wire 类型>` 完全匹配的 TypeSupport。
若不存在，才按 ROS 生成的 DDS 命名约定匹配已加载的 ROS 类型包，例如
`dds:std_msgs::msg::dds_::String_` 对应 `ros2:std_msgs/msg/String`，并使用该包的
`ros2` 反序列化入口读取 CDR。支持 `msg`、`srv` 和 `action` 类型命名；类型包仍需
提供 JSON/YAML 转换。完全匹配的原生 DDS 解码器优先，其格式或解码错误不会触发
回退。话题名称及发现输出仍保留 DDS wire 名称，例如：

```bash
ibmw topic echo rt/chatter --dds --type-support ./libtext_ts.so --once
```

解码和打印在命令线程执行，接收队列最多保留 100 条消息；输出跟不上导致队列满时
明确失败。`--timeout` 只限制发现发布者的等待时间；发布者已发现但不发消息时，
`--once` 仍会等待，可用 `--duration` 限制观察时长。没有收到消息时向 stderr 提示。
当前未实现任意表达式过滤、ROS 仿真时间或任意二进制类型的自动反射。

`ibmw topic hz <topic>` 订阅一个话题，统计观察端实际接收到的频率，每秒输出
平均 Hz、接收间隔的最小值/最大值、总体标准差和窗口消息数。默认使用单调时钟，
窗口为最近 10000 条消息；当前不支持 ROS 仿真时间或消息字段过滤。

```bash
ibmw topic hz /sensor_data                     # 默认：Registry 发现，按能力选择订阅
ibmw topic hz /chatter -r --ros-domain 10       # 独立 ROS 2 订阅端
ibmw topic hz rt/chatter -d --dds-domain 10     # 原生 DDS wire topic 名称
ibmw topic hz /chatter -z --zenoh-domain robot --zenoh-config ./observer.json5
ibmw topic hz /chatter -r -w 100 --duration 10
```

`ibmw topic bw <topic>` 统计观察端收到的序列化消息带宽，复用 `hz` 的订阅方式，
默认窗口为最近 100 条消息。每秒输出带宽及窗口内消息大小的平均值、最小值和
最大值，使用十进制 B/KB/MB 单位（1 KB = 1000 字节）。带宽等于窗口总字节数
除以从窗口第一条消息到当前单调时钟的时长；最后一条消息之后的静默时间也计入，
因此停止发布后带宽会下降。至少收到两条消息才能计算。大小只包含序列化负载，
不包含传输协议头、封装、重传等开销；缓存类型订阅统计生成的序列化负载大小，
不代表消息对象的内存大小。

```bash
ibmw topic bw /sensor_data
ibmw topic bw /chatter -r --ros-domain 10 -w 100 --duration 10
```

`hz` 和 `bw` 每次只接受一个发现来源，不支持 `--all`。话题存在多个发布类型时使用
`--type <type>` 指定 `topic type` 显示的类型名。隐藏话题可直接指定。
`--window/-w` 至少为 2；`--timeout` 设置等待发现发布者的时限（默认 10 秒）；
`--duration` 设置订阅建立后的观察时长，默认持续到 Ctrl+C。尚未接收到消息时
输出提示；频率和带宽受观察端调度和传输丢包影响，不等于业务订阅者的接收结果。

网络模式自动加载安装目录中的对应 transport 库；ROS 2 通用订阅仍要求本机安装
相应消息类型包。需要额外传输配置时传入 `--config-file <yaml>`，只加载其中选定的
`ibmw.extensions.items`、`ibmw.messaging.drivers` 和调度配置。相对路径基于该文件，
domain 始终由上述发现参数及其默认值决定；Zenoh 原生配置优先使用 `--zenoh-config`，
否则使用配置中的 `native_cfg_path`。DDS/ROS 2 默认请求 `best_effort`、`volatile`，
可用 `--qos-reliability`、`--qos-durability` 覆盖。对于 exclusive ownership 的 DDS
发布者，未指定可靠性时自动使用 `reliable`；显式指定 `--qos-reliability best_effort`
会报 QoS 不兼容错误。Registry 模式跟踪发布端增减，并与
`bag record --registry` 共用采集路径选择：优先独立订阅，必要时使用 Runtime Tap。
Registry 发现的 DDS/ROS 2 QoS 按发布端适配；缓存类型订阅保留单发布者和启动前注册限制。
发现 Tap 丢包时报告累计丢包数。

`ibmw topic list`、`info`、`type`、`find` 共用以下发现参数：

| 参数 | 含义 |
|------|------|
| `--registry` | 读取本机运行时登记，覆盖各种传输驱动；未选择来源时默认使用 |
| `-d`、`--dds` | 选择 DDS 发现 |
| `-r`、`--ros2` | 选择 ROS 2 发现 |
| `-z`、`--zenoh` | 选择 Zenoh 发现 |
| `-a`、`--all` | 选择全部四种来源，范围限于各自配置的域 |
| `--domain <id>` | 为选中的 DDS、ROS 2 设置共同 domain |
| `--dds-domain <id>` | 覆盖 DDS domain；否则使用 `--domain` 或 0 |
| `--ros-domain <id>` | 覆盖 ROS 2 domain；否则依次使用 `--domain`、`ROS_DOMAIN_ID`、0 |
| `--zenoh-domain <name>` | Zenoh 业务域（`limit_domain`），默认空域 |
| `--zenoh-config <path>` | Zenoh 原生连接配置 |
| `--registry-path <path>` | 本机注册根目录，默认 `$XDG_RUNTIME_DIR/ibmw`，否则 `/tmp/ibmw-<euid>`。协议数据位于 `v2/`；自定义根目录需为当前用户私有（0700）。 |

本机登记来源在详细输出中标为 `source: registry`，实际传输驱动另列于 `drivers`。

`info <topic>` 与 ROS 2 一样允许直接查询隐藏话题；`list`、`find`、`type`
默认过滤隐藏话题，使用 `--include-hidden-topics` 后包含它们。该开关可放在
子命令前或后，例如 `ibmw topic --include-hidden-topics type /_hidden`。

`info -v` 为每个端点显示类型及可获取的归属信息，本机登记还显示模块名。
多驱动端点的 QoS 按驱动分别展示；有值的策略直接输出，未报告的策略标为
`unknown`，完全没有 QoS 数据时显示 `unavailable (not reported by this source)`。
本机登记使用当前结构的 `module_name` 和 `driver_qos` 字段，不读取旧结构。
组合来源的计数包含各来源的观察结果，不表示跨来源去重后的端点数量。

来源参数可以组合，例如 `list --registry --dds`、`list -d -r --domain 10`。
域与配置参数要求同时选中对应来源。数字域接受 32 位无符号十进制数值，
具体网络实现的取值限制将在对应网络适配器中校验。

DDS 来源已实现（需开启 `IBMW_BUILD_DDS_EXTENSION`）。独立的
`libibmw_discovery_dds.so` 仅在选择 DDS 时加载；只查询本机时不会加载它。
可用 `IBMW_DISCOVERY_PLUGIN_DIR` 覆盖插件查找目录。

DDS 加入指定 domain，通过原生 writer/reader 发现回调收集信息，不订阅业务
数据。结果保留原生 DDS topic/type 名称、端点和 participant GUID、CDR 序列化
格式及已报告的 QoS。

ROS 2 来源需开启 `IBMW_BUILD_WITH_ROS2` 和 `IBMW_BUILD_ROS2_EXTENSION`。
选择 `-r` 时加载独立的 `libibmw_discovery_ros2.so`，通过私有 rclcpp context
和查询节点读取 ROS graph，不初始化 IBMW ROS 2 业务传输扩展，也不订阅业务
数据。该插件需要 ROS 2/rclcpp 运行时，遵循 `RMW_IMPLEMENTATION` 等原生
ROS 环境配置。返回 ROS 逻辑话题名、类型名、端点 GID 和 graph 报告的 QoS；
`topic info -v` 还显示节点名与命名空间。未提供的 DDS participant GUID 和
底层名称保持为空，不根据 RMW GID 或 ROS 名称推测。查询节点关闭 rosout、
参数事件和参数等服务。

```bash
ibmw topic list -r --ros-domain 10
ibmw topic info /chatter -r --ros-domain 10 -v
ibmw topic list -d -r --dds-domain 10 --ros-domain 20
```

Zenoh 来源需开启 `IBMW_BUILD_ZENOH_EXTENSION`。选择 `-z` 时加载独立的
`libibmw_discovery_zenoh.so` 并创建查询 session，CLI 不加载业务传输扩展。
运行中的业务程序需要使用更新后的 `libibmw_zenoh_transport.so`，由它在已有
session 上通过 queryable 提供端点元信息。普通 Zenoh 程序和旧版 IBMW 扩展
不会自动提供这些元信息。

`--zenoh-domain` 必须精确匹配业务 YAML 中的 `limit_domain` 原值；省略时只查
空域，并非全部域。`--zenoh-config` 接受查询端的 Zenoh 原生配置，例如连接
peer/router 的地址；查询 session 必须能到达业务 session。省略配置文件时
使用 Zenoh 默认配置，包括原生 scouting 设置。UDP scouting 用于建立 session
连接，随后通过 IBMW 元信息查询获取逻辑话题名和类型名。

```bash
ibmw topic list -z --zenoh-domain robot --zenoh-config ./observer.json5
ibmw topic info /chatter -z --zenoh-domain robot --zenoh-config ./observer.json5 -v
ibmw topic type /chatter -z --zenoh-domain robot --zenoh-config ./observer.json5
```

统计对象是成功创建的 Zenoh publisher/subscriber，包括未发送消息的端点和
只有订阅者的话题。多个模块回调共用一个原生 subscriber 时只计一个端点，
RPC 内部端点不纳入话题发现。结果提供 session ZID、实例内端点 ID、逻辑
topic/type 和实际 wire key；注册时无法确定的序列化格式和 QoS 保持为空。
查询端在收集窗口内重试，按 revision 替换各实例快照，并移除连续 2 秒未回复的
实例。快照仍是有限时间内的观察，输出时实例可能刚刚退出。

网络查询默认收集 2 秒后返回快照；每次调用独立发现，不使用常驻发现服务。
实际可见范围由 domain、发现配置、安全配置和网络可达性决定。DDS、ROS 2 与
Zenoh 是独立来源，组合查询保留各自端点，不跨来源合并身份。
来源失败时退出码为 1，但组合查询仍输出成功来源的结果。

ROS 2 原生库可能在加载阶段直接退出进程，例如 Humble 中指定了未安装的
`RMW_IMPLEMENTATION`。这种退出发生在适配器运行前，无法作为普通来源错误
捕获，也无法保留其他来源结果；使用 ROS 2 发现前需确保所选 RMW 可用。

`bag record` 默认通过本机 registry 发现话题，按驱动采集能力选择订阅；网络模式
支持 DDS、ROS 2 和 Zenoh：

```bash
ibmw bag record --registry /chatter -o ./registry_bag
ibmw bag record --dds --domain 87 --config-file ./dds-recorder.yaml /native_topic -o ./dds_bag
ibmw bag record --ros2 --domain 87 --config-file ./ros2-recorder.yaml /chatter -o ./ros2_bag
ibmw bag record --zenoh --zenoh-domain robot --config-file ./zenoh-recorder.yaml /chatter -o ./zenoh_bag
```

Registry 发现与采集方式相互独立。Registry 模式根据注册信息中的采集能力和连接配置，
在独立录制运行时建立订阅：按优先级选择 iceoryx、DDS、Zenoh、MQTT、ROS 2，
最后才使用 Runtime Tap。DDS、ROS 2、Zenoh 使用原生序列化订阅；iceoryx、MQTT
使用 TypeSupport 和缓存类型订阅。录制运行时使用独立实例名，不加载业务模块。
同一话题只选择一条覆盖全部发布者的传输路径，避免多驱动重复录制。

缓存类型订阅仅用于单发布者话题，并在驱动启动前注册；运行中可复用已注册的
类型订阅。多发布者、连接配置冲突或无法动态注册时，选择其他可用的直连路径，
否则为每个发布者建立 Tap。UDP、TCP、HTTP、local 通常使用 Tap。Tap 必须由发布端
声明支持，且无法采集绕过普通 Send 的显式 Loan/PublishLoaned 发布。

网络模式不需要发布端的本机注册文件，通过对应发现插件取得订阅地址，再由
录制进程的传输扩展接收序列化消息。同一话题的多个网络发布者共用一个订阅。
网络模式使用录制端自己的配置，不从网络元信息加载扩展或 TypeSupport。

网络录制必须提供录制端自己的配置和包含 schema 的本机 TypeSupport 包：

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

ROS 2 配置使用 `ros2_extension`、`libibmw_ros2_transport.so`、`type: ros2`，
并加载对应 ROS 消息类型的 TypeSupport 包。录制端自动设置独立节点名，使用
独立 ROS context。Zenoh 配置使用 `zenoh_extension`、`libibmw_zenoh_transport.so`、
`type: zenoh`，其扩展 options 示例为：

```yaml
options:
  limit_domain: robot
  native_cfg_path: ./zenoh-client.json5
```

包、扩展、DDS `xml_profile_file` 和 Zenoh `native_cfg_path` 的相对路径以
录制配置文件目录为基准。所选话题缺少 schema 会明确报错。网络元信息中的
路径不会用于加载代码。

| 来源 | 使用的订阅地址 | bag 中的类型／格式 |
|------|----------------|--------------------|
| DDS | 发现的原始 DDS topic/type，不再次转换名称 | `dds:<原始类型名>`／`cdr` |
| ROS 2 | ROS graph 的 topic 和 `pkg/msg/Type` | `ros2:pkg/msg/Type`／`ros2` |
| Zenoh | 元信息中的实际 wire key | 原有逻辑类型／消息封装携带的实际格式 |

Zenoh 从每条收到的消息中取得实际格式，并在写入前匹配 schema。
使用 `--serialization-format auto` 时，只提供对应 schema 的类型包即可；
转换格式还需要 TypeSupport 的序列化函数。收到没有本机 schema 的格式时停止并报错。
尚未收到消息、格式未知的话题不会预先创建存储通道。
DDS、ROS 2 的 reliability/durability 按所有已发现发布者适配，订阅端的 QoS
不会被当作发布能力。发现发布者、类型或 QoS 变化时共用同一套动态采集流程。

`--domain` 用于 DDS／ROS 2：默认取配置中的 `domain_id`，ROS 2 未配置时使用
`ROS_DOMAIN_ID`，否则为 0；命令行与配置同时设置时必须一致。Zenoh 的
`--zenoh-domain` 与配置中的 `limit_domain` 也必须一致。Zenoh 发现和订阅各自
持有会话，默认读取相同 native 配置；使用客户端配置或自动分配的监听端口。
如订阅配置包含固定监听端口或固定会话 ID，用 `--zenoh-config <path>` 明确
指定单独的发现会话配置。

首次收集 2 秒后开始录制，随后复用观察者发现晚加入的发布端。
Zenoh 实例连续 2 秒没有回复元信息查询时从快照移除。
`--no-discovery` 关闭后续发现更新；现有网络订阅仍可接收该话题后加入的发布者。
每次 record 选择一个来源，`-a` 表示该来源的所有话题，`-d` 仍为分包时长。
`--use-sim-time` 适用于本机、ROS 2 和有逻辑 `/clock` 的 Zenoh 来源。

DDS 观察者仍使用所选 domain 的默认发现配置，采集驱动使用录制端传输配置。
原生 DDS 采集仍不支持通用 keyed 类型；bag 保留原始话题名，回放到相同原始
名称的适配不在本次录制改造内。原始 DDS 模式不支持 `/clock` 名称映射。

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
