# ROS2 RPC 示例


## ROS2 RPC Sync


一个使用 ROS2 srv 协议、同步接口、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 srv 协议作为 RPC 服务协议；
- 如何在 Module 模式下使用 Executor、同步 RPC client 与 server 接口；
- 如何使用 local RPC 后端；
- 如何在 Pkg 模式下集成 Module 并启动；


核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_sync_client_module.cc](./managed/normal_rpc_sync_client_module/normal_rpc_sync_client_module.cc)
- [normal_rpc_sync_server_module.cc](./managed/normal_rpc_sync_server_module/normal_rpc_sync_server_module.cc)
- [service.cc](./managed/normal_rpc_sync_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_rpc_sync_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_sync_cfg.yaml)



如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_sync.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例创建以下两个 module：
  - `NormalRpcSyncClientModule`：使用 `work_thread_pool` executor，按配置频率通过同步 Client 接口向 `RosTestRpc` 发送 RPC 请求；
  - `NormalRpcSyncServerModule`：注册 `RosTestRpc` 服务，通过同步 Server 接口提供 echo 功能；
- 本示例将 `NormalRpcSyncClientModule` 与 `NormalRpcSyncServerModule` 分别集成到两个独立 package `ros2_rpc_client_pkg` 与 `ros2_rpc_server_pkg`，并通过配置文件加载到同一个 IBMW 进程；
- 本示例使用 local RPC 后端通信，并配置 `timeout_handle` executor 作为超时 executor；

预期输出（所有子 demo 模式相似）：
```
[INFO] RPC call succeeded, response: <echo of request>
[INFO] RPC call succeeded, response: <echo of request>
...
```
Server 记录每个收到的请求，client 记录每次成功的响应。具体日志格式因子 demo 不同（Sync/Async/Future/Co）而异，但都演示成功的 RPC 往返。



## ROS2 RPC Async


一个使用 ROS2 srv 协议、异步接口、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 srv 协议作为 RPC 服务协议；
- 如何在 Module 模式下使用 Executor、异步 RPC client 与 server 接口；
- 如何使用 local RPC 后端；
- 如何在 Pkg 模式下集成 Module 并启动；



核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_async_client_module.cc](./managed/normal_rpc_async_client_module/normal_rpc_async_client_module.cc)
- [normal_rpc_async_server_module.cc](./managed/normal_rpc_async_server_module/normal_rpc_async_server_module.cc)
- [service.cc](./managed/normal_rpc_async_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_rpc_async_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_async_cfg.yaml)



如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_async.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例创建以下两个 module：
  - `NormalRpcAsyncClientModule`：使用 `work_thread_pool` executor，按配置频率通过异步 Client 接口向 `RosTestRpc` 发送 RPC 请求；
  - `NormalRpcAsyncServerModule`：注册 `RosTestRpc` 服务，通过异步 Server 接口提供 echo 功能；
- 本示例将 `NormalRpcAsyncClientModule` 与 `NormalRpcAsyncServerModule` 分别集成到两个独立 package `ros2_rpc_client_pkg` 与 `ros2_rpc_server_pkg`，并通过配置文件加载到同一个 IBMW 进程；
- 本示例使用 local RPC 后端通信，并配置 `timeout_handle` executor 作为超时 executor；



## ROS2 RPC Future

一个使用 ROS2 srv 协议、future 风格接口、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 srv 协议作为 RPC 服务协议；
- 如何在 Module 模式下使用 Executor、future 风格 RPC client 接口与同步 RPC server 接口；
- 如何使用 local RPC 后端；
- 如何在 Pkg 模式下集成 Module 并启动；



核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_future_client_module.cc](./managed/normal_rpc_future_client_module/normal_rpc_future_client_module.cc)
- [normal_rpc_sync_server_module.cc](./managed/normal_rpc_sync_server_module/normal_rpc_sync_server_module.cc)
- [service.cc](./managed/normal_rpc_sync_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_rpc_future_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_future_cfg.yaml)



如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_future.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例创建以下两个 module：
  - `NormalRpcFutureClientModule`：使用 `work_thread_pool` executor，按配置频率通过 future 风格 Client 接口向 `RosTestRpc` 发送 RPC 请求；
  - `NormalRpcSyncServerModule`：注册 `RosTestRpc` 服务，通过同步 Server 接口提供 echo 功能；
- 本示例将 `NormalRpcFutureClientModule` 与 `NormalRpcSyncServerModule` 分别集成到两个独立 package `ros2_rpc_client_pkg` 与 `ros2_rpc_server_pkg`，并通过配置文件加载到同一个 IBMW 进程；
- 本示例使用 local RPC 后端通信，并配置 `timeout_handle` executor 作为超时 executor；



## ROS2 RPC Co (Coroutine)


一个使用 ROS2 srv 协议、coroutine 接口、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 srv 协议作为 RPC 服务协议；
- 如何在 Module 模式下使用 Executor、coroutine RPC client 与 server 接口；
- 如何使用 coroutine 风格的 RPC filter 功能；
- 如何使用 local RPC 后端；
- 如何在 Pkg 模式下集成 Module 并启动；



核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_co_client_module.cc](./managed/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
- [normal_rpc_co_server_module.cc](./managed/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
- [service.cc](./managed/normal_rpc_co_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_rpc_co_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_co_cfg.yaml)



如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_co.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例创建以下两个 module：
  - `NormalRpcCoClientModule`：使用 `work_thread_pool` executor，按配置频率通过 coroutine Client 接口向 `RosTestRpc` 发送 RPC 请求；
  - `NormalRpcCoServerModule`：注册 `RosTestRpc` 服务，通过 coroutine Server 接口提供 echo 功能；
- 本示例在 RPC Client 与 Server 两侧各注册两个 Filter，用于记录请求日志与测量延迟；
- 本示例将 `NormalRpcCoClientModule` 与 `NormalRpcCoServerModule` 分别集成到两个独立 package `ros2_rpc_client_pkg` 与 `ros2_rpc_server_pkg`，并通过配置文件加载到同一个 IBMW 进程；
- 本示例使用 local RPC 后端通信，并配置 `timeout_handle` executor 作为超时 executor；



## ROS2 RPC Single Pkg


一个使用 ROS2 srv 协议、coroutine 接口、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 srv 协议作为 RPC 服务协议；
- 如何在 Module 模式下使用 Executor、coroutine RPC client 与 server 接口；
- 如何使用 coroutine 风格的 RPC filter 功能；
- 如何使用 local RPC 后端；
- 如何在 Pkg 模式下集成 Module 并启动；



核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_co_client_module.cc](./managed/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
- [normal_rpc_co_server_module.cc](./managed/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
- [service.cc](./managed/normal_rpc_co_server_module/service.cc)
- [ros2_rpc_pkg/pkg_main.cc](./package/ros2_rpc_pkg/pkg_main.cc)



配置文件：
- [examples_cpp_ros2_rpc_single_pkg_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_single_pkg_cfg.yaml)



如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_single_pkg.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例本质上与 **ROS2 RPC Co** 相同，唯一区别是 `NormalRpcCoClientModule` 与 `NormalRpcCoServerModule` 被集成到同一个 package `ros2_rpc_pkg`；

## ROS2 RPC Server App


一个使用 ROS2 协议、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 协议作为 RPC 服务协议；
- 如何在 App 模式下创建 module 并使用 RPC 功能；
- 如何使用 local RPC 后端；
- 如何以 App 模式启动；


核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_ros2_rpc_server_app/main.cc](./standalone/normal_ros2_rpc_server_app/main.cc)
- [normal_rpc_sync_client_module.cc](./managed/normal_rpc_sync_client_module/normal_rpc_sync_client_module.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_rpc_server_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_server_app_cfg.yaml)


如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_server_app.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 在 App 模式下，本示例直接创建 `NormalRpcSyncServerModule`，获取 CoreRef 句柄，注册 `RosTestRpc` 服务，并通过同步 Server 接口提供 echo 功能；
- 本示例使用 local RPC 后端通信；
- 本示例以 App 模式启动；


## ROS2 RPC Client App


一个使用 ROS2 协议、local 后端的 RPC 示例。本 demo 涵盖：
- 如何用 ROS2 协议作为 RPC 服务协议；
- 如何在 App 模式下创建 module 并使用 RPC 功能；
- 如何使用 local RPC 后端；
- 如何以 App 模式启动；


核心代码：
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_ros2_rpc_client_app/main.cc](./standalone/normal_ros2_rpc_client_app/main.cc)
- [normal_rpc_sync_server_module.cc](./managed/normal_rpc_sync_server_module/normal_rpc_sync_server_module.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)



配置文件：
- [examples_cpp_ros2_rpc_client_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_client_app_cfg.yaml)


如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_rpc_client_app.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 在 App 模式下，本示例直接创建 `NormalRpcSyncClientModule`，获取 CoreRef 句柄，按配置频率通过同步 Client 接口向 `RosTestRpc` 发送 RPC 请求；
- 本示例使用 local RPC 后端通信；
- 本示例以 App 模式启动；

---

## ibmw-gen 方式

本 demo 也可以通过 `ibmw-gen` 从
[`ros2_rpc.ibmw.yaml`](./ros2_rpc.ibmw.yaml) 定义生成。

当 YAML 中指定 `ros2_service` 时，ibmw-gen 会生成**真正可编译的** ROS2 RPC
代码 —— 而非仅仅是骨架桩。生成的 server 会注册服务，生成的 client 会在循环
中周期性发送 RPC 调用。你只需要补充服务处理逻辑（server `.cc` 中的 `// TODO`）。

### ibmw-gen 生成 vs 你自己写

| 生成（自动） | 你自己写 |
|-------------------|-----------|
| `CMakeLists.txt` —— 含 ROS2 RPC gencode 链接的构建系统 | `rpc_server_module.cc` 中的服务处理逻辑 |
| `cfg/ros2_rpc_app.yaml` —— 含 `ros2_extension` + `type: ros2` driver 的配置 | `rpc_client_module.cc` 中的请求填充 |
| `cfg/ros2_rpc_pkg.yaml` —— Pkg 模式配置 | |
| `src/ros2_rpc_app/main.cc` —— App 入口 | |
| `src/*_pkg/pkg_main.cc` —— Pkg 加载器 | |
| `src/rpc_server_module/` —— 含服务实现类的 server | |
| `src/rpc_client_module/` —— 含 RPC 循环 + 配置读取的 client | |

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py ros2_rpc.ibmw.yaml -o ros2_rpc_gen

# 构建
source /opt/ros/humble/setup.bash
mkdir -p ros2_rpc_gen/build && cd ros2_rpc_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行（App 模式 —— 需要把 ros2_extension .so 加到 LD_LIBRARY_PATH）
export LD_LIBRARY_PATH="<ibmw_build_dir>/lib:<ibmw_build_dir>:$LD_LIBRARY_PATH"
./ros2_rpc_app cfg/ros2_rpc_app.yaml

# 运行（Pkg 模式）
ibmw run cfg/ros2_rpc_pkg.yaml
```

### 关键 YAML 特性

```yaml
modules:
  - name: RpcServerModule
    type: rpc_server
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc               # ← 触发真实代码生成
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"

  - name: RpcClientModule
    type: rpc_client
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc               # ← 触发真实代码生成
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"
    custom_config:
      rpc_frq: 1.0                         # ← 模块自定义配置
```

`ros2_service` 字段告知 ibmw-gen 生成的代码要 include gencode 头文件
（`RosTestRpc.ibmw_rpc.srv.h`），并使用其中类型化的 `SyncService`/`SyncAccess`
类。若不指定 `ros2_service`，ibmw-gen 退化为带 TODO 标记的骨架桩。

生成的配置自动包含：
- `ros2_extension`，其 `node_name` 由项目命名空间派生
- 与 `type: local` 并列的 `type: ros2` service driver
- 所有 client/server func_name 模式的 `enable_drivers: [ros2, local]`

上面的手写 demo 展示了全部 4 种调用模式（Sync、Async、Future、Coroutine）。
ibmw-gen 版本作为推荐起点生成 Sync 模式。

**注意：** ROS2 RPC 服务类型（`RosTestRpc`）由主 IBMW 构建通过
`ros2_gen_ibmw_cpp_rpc.py` 在外部生成。ibmw-gen 引用预先构建好的
`ros2_rpc_gencode_target` CMake target 进行链接。

### 架构注记：ROS2 后端依赖

ROS2 RPC 互操作需要 `ros2_extension`（它会在 IBMW 进程内运行一个
`rclcpp::Node`）。这是因为 ROS2 服务使用一套构建在 rcl 之上的自定义
请求/响应协议，而非标准 DDS 请求/响应。详见下方
[ROS2 后端依赖分析](#ros2-backend-dependency) 一节。

<a id="ros2-backend-dependency"></a>

## ROS2 后端依赖分析

### IBMW 哪些功能需要 ROS2 后端？

"ROS2 后端"指加载 `ros2_extension`（`libibmw_ros2_transport.so`），
它会创建一个 `rclcpp::Node` 并运行一个 `rclcpp::Executor` spin 线程。
这相当于让 IBMW 进程成为一个运行在 rcl runtime 之内的 ROS2 节点。

| 功能 | 需要的 extension | 是否需要 rcl？ | 说明 |
|---------|-----------------|---------------|-------|
| Pub/sub（IBMW 类型，DDS 线协议） | `dds_extension` | **否** | 纯 FastDDS |
| Pub/sub（经 DDS 的 ROS2 互操作） | `dds_extension` | **否** | 使用 `ros2_compatible: true`，DDS topic/type 命名 |
| Pub/sub（经 rcl 的 ROS2 类型） | `ros2_extension` | 是 | 使用 `rcl_publish`/rcl subscriptions |
| 零拷贝 / loaned message | `dds_extension` | **否** | FastDDS data sharing |
| **ROS2 RPC / Services** | **`ros2_extension`** | **是** | `rcl_service_init`、`rcl_send_request/response` |
| DDS RPC / Services | `dds_extension` | **否** | `type: dds` service driver，纯 FastDDS。参见 `dds_rpc_demo`。 |

**关键洞见：** Pub/sub 的 ROS2 互操作完全可以经纯 DDS 实现（含
`ros2_compatible: true` 的 `dds_extension` 会处理 topic 名前缀
（`rt/`）与 DDS 类型名转换）。**ROS2 服务现在也可以不依赖 rcl 工作**，
通过 `type: dds` service driver 即可（参见 `dds_rpc_demo`）。
`type: ros2` driver 仍然需要 rcl 才能完整兼容 ROS2 服务。

### 为什么 ROS2 RPC 需要 rcl？

ROS2 服务**不**使用标准 DDS 请求/响应（DDS-RPC），而是使用一套构建在 DDS
之上、完全由 rcl/rmw 层管理的自定义协议：

1. **自定义 topic 命名：** 每个服务使用两个 DDS topic：
   - `rq/<node_name>/<service_name>Request`
   - `rr/<node_name>/<service_name>Reply`

2. **自定义关联方式：** 使用 `rmw_request_id_t`（writer GUID + 序列号）
   做请求-响应匹配，而非 DDS correlation ID。

3. **服务类型支持：** 需要 `rosidl_service_type_support_t`，它把请求/响应
   类型支持与服务专属元数据捆绑在一起。

4. **IBMW 对 rcl API 的使用：**
   - Server：`rcl_service_init()`、`rcl_send_response()`，继承
     `rclcpp::ServiceBase`，通过 `node->get_node_services_interface()` 注册
   - Client：`rcl_client_init()`、`rcl_send_request()`，继承
     `rclcpp::ClientBase`，通过 `node->get_node_services_interface()` 注册
   - Executor：`rclcpp::Executor::spin()` 派发收到的请求/响应

### Demo 按后端需求分类

**纯 DDS（无 rcl，无 ros2_extension）：**
- `dds_rpc_demo` —— DDS RPC，使用 `type: dds` service driver
- `ros2_interop_demo` —— 经 DDS 的 ROS2 pub/sub 互操作（`ros2_compatible: true`）
- `ros2_point_demo` —— 经 DDS 的 ROS2 geometry_msgs
- `transport_demo` —— DDS pub/sub
- `dds_native_demo`、`dds_image_demo` —— 原生 DDS 类型
- `loaned_message_demo` —— 经 FastDDS 的零拷贝

**需要 ros2_extension（rcl runtime）：**
- `ros2_rpc` —— 所有子 demo（Sync、Async、Future、Co、SinglePkg、App）
- `ros2_chn` —— 经 rcl 的 ROS2 transport pub/sub（不过这类也可以经
  `ros2_compatible: true` 的 DDS 工作）
