# DDS RPC Demo

演示用纯 DDS service driver（`type: dds`）实现 IBMW DDS RPC 框架 —— 不依赖
rcl/rclcpp。打开 `ros2_compatible_mode: true`（默认）后，DDS topic 和类型名
遵循 ROS2 命名约定，可与原生 ROS2 service client / server 透明互通。

## 生成的内容

ibmw-gen 从 `dds_rpc_demo.ibmw.yaml` 产出：

- 请求/响应消息的 DDS IDL 定义（`Calculator.idl`）
- 用于序列化封装的 RPC gencode（`Calculator.ibmw_rpc.dds.h/cc`）
- 一个处理 `Add` 请求的协程 RPC server module
- 一个周期性发 `Add` 请求的协程 RPC client module
- 含 `dds_extension` 与 `type: dds` service driver 的配置 YAML
- 集成 `DdsIdlGenCode` + `DdsIdlRpcGenCode` 的 CMake 构建

## 生成

```bash
cd dds_rpc_demo
python3 <ibmw>/src/tools/ibmw_gen/ibmw_gen.py dds_rpc_demo.ibmw.yaml -o dds_rpc_demo_gen
```

## 构建

```bash
cd dds_rpc_demo_gen
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw-install-dir>
make -j$(nproc)
```

## 运行（App 模式）

开两个终端：

```bash
# 终端 1：Server
./dds_rpc_server_app cfg/dds_rpc_server_app.yaml

# 终端 2：Client
./dds_rpc_client_app cfg/dds_rpc_client_app.yaml
```

## 运行（Pkg 模式）

```bash
# 终端 1：Server
ibmw run cfg/rpc_server_module_pkg.yaml

# 终端 2：Client
ibmw run cfg/rpc_client_module_pkg.yaml
```

## 期望输出

Server：
```
DDS RPC server registered: func='dds:/dds_rpc_demo.CalculatorService/Add'
DDS RPC server received request: func='', identity seq=(0,1)
Add called
DDS RPC server received request: func='', identity seq=(0,2)
Add called
...
```

Client：
```
DDS RPC client registered: func='dds:/dds_rpc_demo.CalculatorService/Add'
Start RPC loop.
Add OK
Add OK
...
```

## 配置

| 配置文件 | 说明 |
|----------|------|
| `dds_rpc_server_app.yaml` | Server，含 DDS + local service driver |
| `dds_rpc_client_app.yaml` | Client，含 DDS + local service driver |
| `rpc_server_module_pkg.yaml` | Pkg 模式 Server |
| `rpc_client_module_pkg.yaml` | Pkg 模式 Client |

## 涉及概念

- 不依赖 rcl/rclcpp 的纯 DDS RPC
- 带 `ros2_compatible: true` 的 `type: dds` service driver
- ROS2 兼容的 DDS topic 命名（`rq/` / `rr/` 前缀，URL 编码后的名称）
- 面向 DDS RPC 项目的 ibmw-gen 端到端代码生成
- 协程化（`co_await`）的 client / server 模式
- IDL → C++ 的代码生成流水线（`fastddsgen` + `dds_idl_gen_rpc`）

## ROS2 互操作

打开 `ros2_compatible_mode: true` 后，service 可被 ROS2 发现：

```bash
ros2 service list
# /dds_5Frpc_5Fdemo_2ECalculatorService/Add
```

完整的双向 ROS2 service 调用要求 ROS2 一侧有相匹配的 `.srv` 类型。基于通用
`RosRpcWrapper.srv` 的范例可参考核心 DDS RPC driver 测试
`build/cfg/dds_rpc_ros2_interop_*`。
