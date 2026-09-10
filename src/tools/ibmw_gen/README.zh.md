# ibmw-gen —— IBMW 项目代码生成器

`ibmw-gen` 从单个 `.ibmw.yaml` 定义文件生成完整、可构建的 IBMW 项目。它会产生：

- **Module 脚手架**（`.h` + `.cc`），含生命周期钩子和 TODO 标记
- **App 入口**（`main.cc`），用于独立可执行文件
- **Pkg 加载器**（`pkg_main.cc`），用于 `ibmw run`（共享库模式）
- **YAML 配置文件**，覆盖 App 模式和 Pkg 模式
- **CMakeLists.txt**，含正确的 target 链接和 ROS2/DDS 集成
- **自定义 DDS 类型头文件**（C++ struct + 内联 FastCDR 序列化）
- **JSON 类型定义**（如已配置）

ROS2 interface 迁移工作流（`ibmw-gen import`）请先阅读
[ROS2 Interface Import User Guide](../../../docs/guides/ros2-interface-import-user-guide.md)。

## 快速开始

```bash
export IBMW_GEN="<ibmw-source-dir>/src/tools/ibmw_gen/ibmw_gen.py"
export IBMW_BUILD_DIR="<ibmw-build-dir>"

# 1. 编写 .ibmw.yaml（见下文示例）

# 2. 生成
python3 "$IBMW_GEN" my_project.ibmw.yaml -o my_project_gen

# 3. 构建
mkdir -p my_project_gen/build && cd my_project_gen/build
cmake .. -DCMAKE_PREFIX_PATH="$IBMW_BUILD_DIR"
make -j$(nproc)

# 4. 运行（App 模式）
./my_app cfg/my_app.yaml

# 5. 运行（Pkg 模式）
ibmw run cfg/my_app_pkg.yaml
```

## 用法

```
ibmw-gen [-h] [-o OUTPUT_DIR] [--force] [--dry-run] [--only CATEGORIES] [-v] [--version] yaml_file
```

| Flag | 说明 |
|------|------|
| `yaml_file` | `.ibmw.yaml` 项目定义文件路径 |
| `-o`, `--output-dir` | 输出目录（默认：`./<project.name>/`） |
| `--force` | 覆盖**所有**文件，包括用户编辑过的 Module |
| `--dry-run` | 仅打印将生成的文件，不实际写入 |
| `--only` | 仅生成指定类别：`types,modules,apps,cmake,configs,schema,all` |
| `-v`, `--verbose` | 详细输出 |
| `--version` | 显示版本 |

## 自动生成 vs 用户编写

这是 ibmw-gen 的核心理念：**工具生成模板代码；你编写业务逻辑。**

### 自动生成（重新运行时会被覆盖）

| 文件 | 说明 |
|------|------|
| `CMakeLists.txt` | 构建系统，含所有 target 与依赖 |
| `cfg/*.yaml` | App 模式与 Pkg 模式 YAML 配置 |
| `src/<app>/main.cc` | App 模式入口（初始化框架 + 运行） |
| `src/<app>_pkg/pkg_main.cc` | Pkg 模式共享库加载器（每个 app 一个） |
| `protocol/<message>.h` | 自定义 DDS 类型头文件（struct + 内联 CDR） |
| `src/json_custom_types.h` | JSON 类型定义 |

### 仅首次生成，之后归你编辑（重新运行时跳过）

| 文件 | 说明 |
|------|------|
| `src/<module>/<module>.h` | Module 头文件，含类声明 |
| `src/<module>/<module>.cc` | Module 实现，含 TODO 标记 |

这些文件包含 `// TODO(ibmw-gen)` 标记，你需要在此添加业务逻辑。后续运行
`ibmw-gen` 时这些文件会被**跳过**以保留你的修改。如需覆盖，使用 `--force`。

## ROS2 Interface Import：Imported `.srv` Runtime 工作流

`ibmw-gen import --ros2-src ... --artifact-only` 可以把 source-mode ROS2
interface package 转成 rosidl-first artifact package。若输入包含 `.srv`，生成
目录还会包含可选的 `<package>_imported_srv_runtime.ibmw.yaml`。把这个 YAML
再次交给 `ibmw-gen`，即可生成使用 imported ROS2 service type support 的 IBMW
server/client modules。

最小流程：

```bash
export IBMW_GEN="<ibmw-source-dir>/src/tools/ibmw_gen/ibmw_gen.py"

# 1. 准备 ROS2 interface package 形状的目录。
mkdir -p /tmp/set_mode_interfaces/srv
cat > /tmp/set_mode_interfaces/srv/SetMode.srv <<'EOF'
string mode
bool enable
---
bool accepted
string reason
EOF

# 2. 生成 rosidl artifact package 和可选 IBMW runtime YAML。
python3 "$IBMW_GEN" import \
  --ros2-src /tmp/set_mode_interfaces \
  --artifact-only \
  --package-name set_mode_interfaces \
  --output-dir /tmp/set_mode_interfaces_gen \
  --target dds_ros2

# 3. 用 ROS2/colcon build/install artifact package，使 C++ service types
#    和 rosidl_typesupport_cpp targets 对 IBMW runtime project 可见。
mkdir -p /tmp/ws/src
cp -a /tmp/set_mode_interfaces_gen /tmp/ws/src/set_mode_interfaces
colcon build --base-paths /tmp/ws/src --merge-install

# 4. 用 emitted skeleton YAML 生成 IBMW runtime project。
python3 "$IBMW_GEN" \
  /tmp/set_mode_interfaces_gen/set_mode_interfaces_imported_srv_runtime.ibmw.yaml \
  -o /tmp/set_mode_runtime --force
```

生成的 runtime modules 会保留业务 user-region markers：

```cpp
// Server user region：设置 response 并返回 RPC status。
// IBMW_RPC_SERVER_HANDLER_BODY_SET_MODE_BEGIN
rsp.accepted = req.enable && req.mode == "auto";
rsp.reason = rsp.accepted ? "accepted:auto" : "rejected";
return ibmw::rpc::Status();
// IBMW_RPC_SERVER_HANDLER_BODY_SET_MODE_END

// Client user region：在 generated call 前填充 request。
// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_MODE_BEGIN
req.mode = "auto";
req.enable = true;
// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_MODE_END

// Client user region：成功 call 后消费/验证 response。
// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_MODE_BEGIN
MW_INFO("SetMode response accepted={}, reason={}", rsp.accepted, rsp.reason);
// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_MODE_END
```

关键行为与边界：

- generated runtime YAML 会为 server/client modules 设置
  `ros2_imported_srv_adapter: true` 和 `ros2_service_name: /<snake_service>`。
- generated app config 会把 IBMW canonical function name
  `ros2:/<package>/srv/<Service>` remap 到友好 ROS2 service name，例如
  `/set_mode`；原生 ROS2 CLI/用户应调用这个友好名。
- server/client adapters 是 typed adapters，使用 imported package 的
  `__rosidl_typesupport_cpp` target 和 `ibmw::GetRos2MessageTypeSupport<T>()`。
- artifact package 本身仍是 rosidl package；runtime adapter header 面向
  IBMW-aware consumer target，需要 C++20、IBMW public API 和 ROS2 type support
  依赖。
- generator 不推断任意业务语义；request/response 处理保留在 user regions。

live 验收脚本：

```bash
bash scripts/live_ros2_imported_srv_acceptance.sh
```

该脚本使用 custom source-mode `SetMode.srv` 和 friendly `/set_mode` service
name 验证双向互通：

- stock ROS2 CLI -> generated IBMW server
- generated IBMW client -> stock ROS2 Python server

## ROS2 Interface Import：JSON Scaffold 工作流

`ibmw-gen import --target json` 也可以为 plain-compatible ROS2 `.msg` 生成
JSON handoff scaffold。这个路径面向迁移团队：先复用已有 ROS2 message 合同，
生成 C++ JSON structs，再用 emitted scaffold YAML 生成普通 IBMW JSON
publisher/subscriber modules。

最小流程：

```bash
export IBMW_GEN="<ibmw-source-dir>/src/tools/ibmw_gen/ibmw_gen.py"
export IBMW_INSTALL_PREFIX="<ibmw-install-prefix>"
export IBMW_NET_INSTALL_PREFIX="<net-enabled-ibmw-install-prefix>"

# 1. 准备 ROS2 interface package 形状的目录。
mkdir -p /tmp/demo_interfaces/msg
cat > /tmp/demo_interfaces/msg/Plain.msg <<'EOF'
float64 x
float64 y
int32[3] rgb
EOF

# 2. 导入 ROS2 interface，并请求 JSON backend handoff artifacts。
python3 "$IBMW_GEN" import \
  --ros2-src /tmp/demo_interfaces \
  --artifact-only \
  --package-name demo_interfaces \
  --output-dir /tmp/demo_interfaces_gen \
  --target json

# 关键生成物：
#   /tmp/demo_interfaces_gen/demo_interfaces_json_types.hpp
#   /tmp/demo_interfaces_gen/demo_interfaces_json_runtime_scaffold.ibmw.yaml
#   /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json

# 3. 用 scaffold YAML 生成普通 IBMW project。
python3 "$IBMW_GEN" \
  /tmp/demo_interfaces_gen/demo_interfaces_json_runtime_scaffold.ibmw.yaml \
  -o /tmp/demo_json_runtime --force

# 4. 用已安装的 IBMW prefix 配置/编译 generated project。
cmake -S /tmp/demo_json_runtime -B /tmp/demo_json_runtime_build \
  -DCMAKE_PREFIX_PATH="$IBMW_INSTALL_PREFIX;/opt/ros/humble" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/demo_json_runtime_build -j$(nproc)
```

生成的 JSON type header 包含普通 C++ structs、
`NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(...)`，以及 deterministic
`kTypeName`，例如 `demo_interfaces/msg/Plain`。scaffold YAML 使用
`ros2_import_json_types_header` 和 `ros2_import_json_types_include_dir`，使第二次
`ibmw-gen` 生成的 modules 可以正确 include imported type header。

可复现验证入口：

```bash
# 复用已有 IBMW install prefix，验证更快。
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_INSTALL_PREFIX" \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_verify

# 或者让 lane 在 work dir 下构建最小 IBMW prefix。
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_verify

# 若 generated migration manifest 已选择 json_scaffold，也可以直接 --manifest：
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_INSTALL_PREFIX" \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json \
  /tmp/ibmw_json_verify

# 可选非 local smoke：通过 HTTP publisher/subscriber 双进程发送指定 case。
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_NET_INSTALL_PREFIX" \
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_CASES=all \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_verify

# 若不设置 IBMW_JSON_SMOKE_IBMW_PREFIX，HTTP smoke 会先在 work dir 下构建
# 带 net extension 的 IBMW prefix，再运行矩阵。
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_CASES="plain bounded" \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_verify_build_prefix

# 可选 installed-prefix reference HTTP smoke。reference cases 必须同时由
# IBMW_JSON_SMOKE_REFERENCE_CASES 生成。
IBMW_JSON_SMOKE_IBMW_PREFIX="$IBMW_NET_INSTALL_PREFIX" \
IBMW_JSON_SMOKE_REFERENCE_CASES=all \
IBMW_JSON_SMOKE_HTTP_2PROC=1 \
IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES=all \
VERIFY_LANES=json_scaffold \
bash scripts/verify_ros2_interface_import_live.sh /tmp/ibmw_json_http_reference_verify
```

在 manifest 模式下，wrapper 会先检查 `backend_generated_files` 是否列出
JSON types header 与 runtime scaffold，并确认这些文件存在，然后才运行通用
scaffold smoke。

CI 友好的性能边界检查可运行 manifest-only guard：

```bash
VERIFY_LANES=perf_boundary \
bash scripts/verify_ros2_interface_import_live.sh \
  --manifest /tmp/demo_interfaces_gen/demo_interfaces_migration_manifest.json \
  /tmp/ibmw_perf_boundary_verify
```

该 lane 会在 work directory 输出 `perf_boundary_summary.json`，检查
ROS2-compatible DDS lane 继续保持 rcl-free，`dds_native` / `iceoryx` 不会被
metadata-only artifact 提升状态，并要求 zero-copy candidate 在未来 lowering
声明 support 前具备 deterministic eligibility 分类。它不修改 runtime hot path。
当 migration manifest 生成了 `dds_native` 或 `iceoryx` metadata sidecar 时，
会自动选择该 lane。
summary 中的 `zero_copy_sized_interfaces` 会统计 metadata 已包含 deterministic
`max_serialized_size_bytes` bound 的 `iceoryx` eligible interfaces。
请求 `iceoryx` 时，import 还会输出 `<pkg>_iceoryx_lowering_plan.json`；该文件是
metadata-only plan，用于记录 eligible/blocked interfaces、未来 type/module names
以及 compile/runtime promotion gate，不代表已经生成 runtime lowering。
请求 `dds_native` 时，import 会输出 `<pkg>_dds_native_lowering_plan.json`，记录
eligible/degraded interfaces、未来 IDL/schema/type artifact names，以及
schema/IDL compile-smoke 到 runtime promotion gate；它同样只是 plan-only。

`.msg`、`.srv` 与 `.action` live lanes 在 matrix 成功后会输出 summary artifacts：
`msg_live_matrix_summary.json`、`imported_srv_live_matrix_summary.json`、
`reference_srv_live_matrix_summary.json` 与 `action_runtime_matrix_summary.json`。
这些文件用于 CI/人工快速审计；详细排查仍以完整 lane logs 为准。
`scripts/verify_ros2_interface_import_live.sh` 现在会在对应 live lane 完成后检查
这些 summary artifacts，因此 lane 不能在缺少机器可读证据的情况下 PASS。Action
summary 会记录稳定的 `action_type` 与 `action_name` 字段，便于 CI dashboard
统计覆盖。

剩余 optional coverage 作为 evidence-only 队列继续推进：在当前 23-case
real-prefix reference `.msg` local/HTTP matrix 之外继续扩 case；增加更多
Nav2/MoveIt-style action runtime-template cases；通过显式 case selector 扩展 JSON
non-local transport 覆盖；并保持 nested plain size bounds 对 native backend plans
可用。没有对应 gate 前，不提升 backend 状态，也不触碰 hot path。

当前已验证的 JSON scaffold 覆盖：

- primitive scalar fields：`bool`、有符号/无符号整数、`float32`、`float64`
- fixed arrays：如 `int32[3]`、`uint8[4]`、`float64[2]`
- strings 与 bounded/unbounded sequences：如 `string<=16`、`uint8[<=8]`、`uint8[]`
- 同 package nested plain message fields
- source/workspace 跨 package nested message fields
- reference-existing 跨 package nested type/scaffold bundles，并包含
  `std_msgs/msg/Header -> builtin_interfaces/msg/Time` local runtime smoke
- 当 `/opt/ros/humble` 可用时，真实 installed-prefix reference-existing
  `geometry_msgs/msg/PoseStamped`、`nav_msgs/msg/Odometry` 与
  `sensor_msgs/msg/Imu`，以及 `geometry_msgs/msg/TransformStamped`、
  `geometry_msgs/msg/Twist`、`sensor_msgs/msg/LaserScan`、`nav_msgs/msg/Path`、
  `sensor_msgs/msg/PointCloud2`、`sensor_msgs/msg/NavSatFix`、
  `geometry_msgs/msg/PoseWithCovarianceStamped`，以及
  `geometry_msgs/msg/PointStamped`、`geometry_msgs/msg/Vector3Stamped`、
  `geometry_msgs/msg/WrenchStamped`、`geometry_msgs/msg/AccelStamped`、
  `sensor_msgs/msg/Image`，以及 `sensor_msgs/msg/CameraInfo`、
  `sensor_msgs/msg/JointState`、`sensor_msgs/msg/BatteryState`、
  `sensor_msgs/msg/Range`、`sensor_msgs/msg/FluidPressure`、
  `sensor_msgs/msg/Temperature`、`nav_msgs/msg/OccupancyGrid`、
  `nav_msgs/msg/MapMetaData` local runtime smoke；可用
  `IBMW_JSON_SMOKE_REFERENCE_CASES=all` 运行该矩阵。该覆盖扩展只增加验证
  coverage，不增加 runtime hot-path 工作。
- generated publisher/subscriber modules、package `.so`、app binaries
- 同进程 package-mode local transport smoke，日志中可看到 publish 和 receive
- 可选 HTTP 双进程 package-mode smoke；覆盖当前全部 generated cases：
  `plain`、`scalars`、`fixed_array`、`bounded`、`dynamic`、`nested_detail`、
  `nested_top`、`external_stamped`
- verification artifact：`json_scaffold_generated_shape_matrix_summary.json`，
  输出在 `json_scaffold` work directory，记录 built-in generated shape local
  runtime evidence 与可选 HTTP evidence 路径
- 可选真实 installed-prefix reference HTTP 双进程 smoke：通过
  `IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES=all` 覆盖 `pose_stamped`、`odometry`、
  `imu`、`transform_stamped`、`twist`、`laser_scan`、`path`、`point_cloud2`、
  `nav_sat_fix`、`pose_with_covariance_stamped`、`point_stamped`、
  `vector3_stamped`、`wrench_stamped`、`accel_stamped`、`image`、
  `camera_info`、`joint_state`、`battery_state`、`range`、`fluid_pressure`、
  `temperature`、`occupancy_grid`、`map_meta_data`
- verification artifact：`json_scaffold_reference_matrix_summary.json`，输出在
  `json_scaffold` work directory，记录 selected/generated/skipped case 状态、
  local runtime evidence 与可选 HTTP evidence 路径
- unified wrapper 会在 lane 结束后校验两个 JSON summary artifacts，包括 selected
  totals、generated/local pass consistency，以及可选 HTTP requested/pass totals
- `route_b` wrapper lane 会校验 `route_b_target_artifact_audit.json`，作为
  AArch64 target artifacts 与 host-prefix leak 的 machine-readable evidence
- CI 可通过 `IBMW_VERIFY_LANE_TIMEOUT_SECONDS` 或
  `IBMW_VERIFY_ROUTE_B_TIMEOUT_SECONDS` 等 per-lane override 限制 delegated
  wrapper lane 耗时

当前边界：

- `json` 仍报告为 `degraded`，不是完整 `supported`。
- import 阶段输出的是 handoff scaffold；不宣称任意 ROS2 message shape 的完整
  JSON runtime lowering 已自动完成。
- HTTP inter-process smoke 目前是 opt-in，通过 `IBMW_JSON_SMOKE_HTTP_CASES`
  选择 generated cases；installed-prefix reference HTTP cases 通过
  `IBMW_JSON_SMOKE_HTTP_REFERENCE_CASES` 单独选择，且必须同时出现在
  `IBMW_JSON_SMOKE_REFERENCE_CASES` 中；更广泛的非 local JSON 覆盖仍是后续工作。
- 未设置 `IBMW_JSON_SMOKE_IBMW_PREFIX` 时，HTTP smoke 会用
  `IBMW_BUILD_NET_EXTENSION=ON` 构建本地 prefix；速度较慢，但不再要求预置
  net-enabled install。
- 该 JSON lane 的 generated IBMW runtime 路径不使用 `rcl`、`rclcpp`、`rclpy`。
- zero-copy / no-memcpy 声明不能从 JSON 或 metadata 覆盖中推导；`iceoryx` 与
  native DDS promotion 必须先通过 `perf_boundary` guard，再补 backend-specific
  compile/runtime evidence。
- `max_serialized_size_bytes` 是 packed ROS2-IR metadata bound，不是 C++
  `sizeof(T)`；runtime chunk sizing 仍必须依赖 backend-specific compile/runtime
  evidence。
- 已解析的 nested plain `.msg` dependencies 现在会汇总进 diagnosis 与
  `dds_native` / `iceoryx` lowering plans 的 size bound。

## .ibmw.yaml Schema 参考

### project（必需）

```yaml
project:
  name: my_project          # 项目名（用作 CMake 项目名）
  namespace: my::namespace   # 生成代码的 C++ namespace
```

### modules（必需）

定义项目中的 Module。每个 Module 会成为一个 C++ 类，含 `Setup(Runtime)`、
`Run()`、`Stop()` 生命周期方法。

**Module 类型：**

| 类型 | 说明 | Transport |
|------|------|-----------|
| `bare` | 无 Transport，无 RPC，纯生命周期 Module | 无 |
| `publisher` | 在 Topic 上发布消息 | DDS、ROS2 或 JSON |
| `subscriber` | 订阅 Topic 消息 | DDS、ROS2 或 JSON |
| `rpc_server` | RPC Service Server | DDS、ROS2 或 JSON |
| `rpc_client` | RPC Service Client | DDS、ROS2 或 JSON |
| `logger` | 含 Executor 支持的日志 Demo Module | 无 |
| `loaned_publisher` | 零拷贝 Publisher，使用 `LoanedMessage` / iceoryx `LoanedSample` | DDS（FastDDS DataSharing）或 iceoryx |
| `loaned_subscriber` | 零拷贝 Subscriber，直接消费 Loaned SHM 样本 | DDS 或 iceoryx |
| `direct_ros2_publisher` | 底层 ROS2 DDS Publisher（无 RCL） | DDS |
| `direct_ros2_subscriber` | 底层 ROS2 DDS Subscriber（无 RCL） | DDS |
| `ros2_interop_publisher` | *（`direct_ros2_publisher` 的 Deprecated 别名。）* | DDS |
| `ros2_interop_subscriber` | *（`direct_ros2_subscriber` 的 Deprecated 别名。）* | DDS |

**示例（DDS Pub/Sub）：**
```yaml
modules:
  - name: PublisherModule
    type: publisher
    transport: dds
    topic: hello_topic
    message_type: ExampleEventMsg

  - name: SubscriberModule
    type: subscriber
    transport: dds
    topic: hello_topic
    message_type: ExampleEventMsg
```

**示例（ROS2 Transport）：**
```yaml
modules:
  - name: MyPublisher
    type: publisher
    transport: ros2
    topic: test_topic
    message_type: "example_ros2::msg::RosTestMsg"
    ros2_msg_include: "example_ros2/msg/ros_test_msg.hpp"
    ros2_package: example_ros2
```

**示例（ROS2 RPC，含真实代码生成）：**
```yaml
modules:
  - name: RpcServerModule
    type: rpc_server
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"

  - name: RpcClientModule
    type: rpc_client
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"
    custom_config:
      rpc_frq: 1.0
```

指定 `ros2_service` 时，ibmw-gen 会生成**真实可编译**的 RPC 代码（带类型的
service 实现、使用 SyncAccess 的 client 循环、通过 yaml-cpp 读取配置）。
不指定时，会生成带 TODO 标记的骨架。

生成的配置自动包含 `ros2_extension` 加载和 `type: ros2` Service Driver 配置。

**示例（DDS RPC —— 纯 FastDDS，无 RCL）：**
```yaml
modules:
  - name: RpcServerModule
    type: rpc_server
    transport: dds
    dds_rpc_service: CalculatorService
    dds_rpc_namespace: dds_rpc_demo
    dds_rpc_methods: [Add]
    dds_rpc_gencode_target: "dds_rpc_demo::dds_rpc_demo_dds_idl_rpc_gencode"

  - name: RpcClientModule
    type: rpc_client
    transport: dds
    dds_rpc_service: CalculatorService
    dds_rpc_namespace: dds_rpc_demo
    dds_rpc_methods: [Add]
    dds_rpc_gencode_target: "dds_rpc_demo::dds_rpc_demo_dds_idl_rpc_gencode"
    custom_config:
      rpc_frq: 1.0
```

生成的配置包含 `dds_extension` 加载和 `type: dds` Service Driver。当
`ros2_compatible_mode: true`（默认）时，DDS Topic 遵循 ROS2 命名规范，可与
原生 ROS2 节点透明互操作。

**示例（bare）：**
```yaml
modules:
  - name: MyModule
    type: bare
    log_level: INFO           # 可选：默认 Module 日志级别
    executor_name: my_exec    # 可选：绑定到某个 Executor
```

### apps（必需）

定义可运行的应用。每个 App 包含一个或多个 Module。

```yaml
apps:
  - name: my_app
    type: pubsub              # pubsub | rpc | bare
    modules: [PublisherModule, SubscriberModule]
```

**Per-app Executor**（可选）：覆盖项目级 Executor，仅对特定 App 生效：

```yaml
apps:
  - name: executor_app
    type: bare
    modules: [MyModule]
    executors:
      - name: work_executor
        type: thread_pool
        options:
          thread_num: 2
```

**Config 变体**（可选）：从同一个 App 生成多份不同设置的 YAML 配置：

```yaml
apps:
  - name: logging_app
    type: bare
    modules: [LoggerModule]
    config_variants:
      - suffix: rotate_file
        logging:
          backends:
            - type: rotate_file
              options:
                path: ./log
                max_file_size_m: 4
```

### dds_types（可选）

引用 IBMW 主构建中由 IDL 生成的 DDS 类型。

```yaml
dds_types:
  namespace: ibmw::protocols::example
  groups:
    - name: ExampleEvent
      idl_gencode_target: ibmw::schema::example_idl_gencode
      messages:
        - name: ExampleEventMsg
          fields:
            - { name: msg, type: string }
            - { name: num, type: int32 }
```

#### `dds_types.rpc_services` —— ROS2 兼容的 DDS RPC

定义带原生 `.srv` 支持的 DDS RPC Service。`srv:` 字段启用自动派生 ROS2 DDS
类型名，并实现与标准 ROS2 service 的线协议级互操作。

```yaml
dds_types:
  rpc_services:
    - name: AddTwoIntsService
      group: AddTwoInts
      srv: example_interfaces/srv/AddTwoInts  # 启用 ROS2 互操作
      # 可选覆盖（未设置时自动从 srv 派生）：
      # ros2_service_name: /custom_name
      # req_dds_type_name: example_interfaces::srv::dds_::AddTwoInts_Request_
      # rsp_dds_type_name: example_interfaces::srv::dds_::AddTwoInts_Response_
      methods:
        - name: AddTwoInts
          request: AddTwoInts_Request
          response: AddTwoInts_Response
```

| 字段 | 必需 | 说明 |
|------|------|------|
| `name` | 是 | 生成代码中使用的 Service 名 |
| `group` | 是 | IDL group 名，用于类型查找 |
| `srv` | 否 | ROS2 `.srv` 包路径（如 `example_interfaces/srv/AddTwoInts`）。设置后自动计算 `req_dds_type_name`、`rsp_dds_type_name`、`ros2_service_name` |
| `ros2_service_name` | 否 | 覆盖 ROS2 service topic 名（未设置时自动从 `srv` 派生） |
| `req_dds_type_name` | 否 | 覆盖 request 的 DDS 类型名 |
| `rsp_dds_type_name` | 否 | 覆盖 response 的 DDS 类型名 |
| `methods` | 是 | RPC method 列表（每个 `.srv` 一个） |

### custom_dds_types（可选）

直接定义自定义 DDS 类型（无需 IDL）。ibmw-gen 会生成 C++ struct 和 FastCDR
序列化代码。

```yaml
custom_dds_types:
  namespace: "my::namespace"
  constants:
    - { name: kBufferSize, type: uint64, value: 1048576 }
  enums:
    - name: Encoding
      underlying_type: uint32
      values:
        - { name: RGB8, value: 0 }
        - { name: GRAY8, value: 2 }
  messages:
    - name: Header
      constructor_args: true
      fields:
        - { name: frame_id, type: uint64, default: "0" }
        - { name: timestamp, type: float64, default: "0.0" }
    - name: ImageMsg
      constructor_args: true
      fields:
        - { name: header, type: Header }          # 嵌套 message
        - { name: encoding, type: Encoding }       # 枚举类型
        - { name: data, type: "array<uint8, kBufferSize>" }
```

**支持的字段类型：** `bool`、`int8`、`int16`、`int32`、`int64`、`uint8`、
`uint16`、`uint32`、`uint64`、`float32`、`float64`、`string`、
`array<T, N>`、`vector<T>`、用户自定义枚举类型，以及之前定义过的其他 message
类型（嵌套 struct）。

**嵌套 message 类型：** 字段可引用同一 `custom_dds_types` 段中定义的其他
message。支持以下形式：

- 直接嵌套：`{ name: header, type: Header }`
- Vector：`{ name: objects, type: "vector<SceneObject>" }`
- 定长数组：`{ name: points, type: "array<Vec3, 4>" }`

Message 之间可自由引用（前向引用会自动解析），但循环引用（A 依赖 B 依赖 A）
会被拒绝。

### executors（可选）

定义所有 App 共享的项目级 Executor（除非被 per-app 覆盖）。

```yaml
executors:
  - name: work_executor
    type: thread_pool
    options:
      thread_num: 2
  - name: strand_executor
    type: strand
    options:
      bind_thread_pool_executor_name: work_executor
```

**Executor 类型：** `thread_pool`、`strand`、`dedicated`、`timer`、
`parallel`、`time_manipulator`

**实时选项（thread_pool）：**
```yaml
executors:
  - name: rt_thread
    type: thread_pool
    options:
      thread_num: 1
      thread_sched_policy: "SCHED_FIFO:80"
      thread_bind_cpu: [0, 1]
      timeout_alarm_threshold_us: 100
```

### dds_driver（可选）

配置 DDS Extension Driver 的 ROS2 互操作。

```yaml
dds_driver:
  domain_id: 0
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"
```

### logging（可选）

配置日志后端与级别。

```yaml
logging:
  core_level: INFO
  backends:
    - type: console
    - type: rotate_file
      options:
        path: ./log
        filename: app.log
        max_file_size_m: 4
        max_file_num: 10
```

### json_types（可选）

定义可 JSON 序列化的类型。生成器会同时生成：

- `src/json_custom_types.h`：C++ 类型、JSON 序列化和 JSON Schema。
- `src/json_type_support_pkg_main.cc`：TypeSupport/SchemaSupport 动态包入口。
- `<project>_json_ts`：可由 Bag 的 `TypeSupportPkgLoader` 加载的共享库目标。

```yaml
json_types:
  messages:
    - name: ConfigData
      fields:
        - { name: name, type: string }
        - { name: value, type: int32 }
```

## 生成的文件结构

对于一个典型的 `my_project`（含一个 publisher + 一个 subscriber，使用
`custom_dds_types`）：

```
my_project_gen/
  my_project.ibmw.yaml          # 输入文件副本（用于重新生成）
  CMakeLists.txt                 # 构建系统
  cfg/
    my_app.yaml                  # App 模式配置
    my_app_pkg.yaml              # Pkg 模式配置（每个 App 一个 Pkg）
  protocol/
    my_message.h                 # 自定义类型头文件（struct + CDR）
  src/
    json_custom_types.h            # JSON 类型及内嵌 JSON Schema
    json_type_support_pkg_main.cc  # JSON TypeSupport/SchemaSupport 动态包入口
    my_app/
      main.cc                    # App 入口（自动生成）
    publisher_module/
      publisher_module.h         # Module 头文件（你来编辑）
      publisher_module.cc        # Module 实现（你来编辑）
    subscriber_module/
      subscriber_module.h        # Module 头文件（你来编辑）
      subscriber_module.cc       # Module 实现（你来编辑）
    my_app_pkg/
      pkg_main.cc                # Pkg 加载器（自动生成，每个 App 一个）
```

**Pkg 命名规则：** App 仅含一个 Module 时，Pkg 名为 `<module>_pkg`；含多个
Module 时，Pkg 名为 `<app>_pkg`（若 App 名以 `_app` 结尾，则为
`<app_without_app_suffix>_pkg`）。

## 工作流

1. **定义** —— 在 `.ibmw.yaml` 文件中定义项目
2. **生成** —— 运行 `ibmw-gen`
3. **编辑** —— 编辑 Module 的 `.h`/`.cc` 文件，填充 TODO 标记
4. **构建** —— 用 CMake 构建
5. **运行** —— App 模式或 Pkg 模式
6. **迭代** —— 修改 YAML 后重新运行 `ibmw-gen`；你的 Module 编辑会被保留

## 示例

`src/samples/cpp/` 下每个 Demo 都有 `.ibmw.yaml` 文件，对其运行 `ibmw-gen`
即可生成对应项目：

| Demo | ibmw.yaml 特性 | Module 类型 |
|------|---------------|-------------|
| `quickstart` | 最小 bare Module | `bare` |
| `transport_demo` | 使用 IDL 类型的 DDS Pub/Sub | `publisher`、`subscriber` |
| `ros2_chn` | ROS2 message Transport | `publisher`、`subscriber` |
| `ros2_rpc` | ROS2 RPC server/client（带真实代码） | `rpc_server`、`rpc_client` |
| `dds_native_demo` | 自定义 DDS 类型 | `publisher`、`subscriber` |
| `dds_image_demo` | 自定义类型 + enum + array | `publisher`、`subscriber` |
| `logging_demo` | Logging 后端 + config_variants | `logger` |
| `config_param_demo` | Executor + Parameter API | `bare` |
| `ros2_interop_demo` | ROS2 DDS 互操作（无 RCL） | `ros2_interop_publisher/subscriber` |
| `ros2_point_demo` | ROS2 geometry_msgs + 零拷贝 | `ros2_interop_publisher/subscriber` |
| `executor_demo` | Per-app Executor + 实时 | `bare`（5 个 Module） |
| `dds_rpc_demo` | 纯 DDS RPC，原生 `.srv` + ROS2 互操作 | `rpc_server`、`rpc_client` |
| `multi_driver_demo` | 单 Module 通过 `IBMW_USE_DRIVERS(...)` 同时挂载 **两个**驱动（DDS + Iceoryx） | `bare`（含 multi-driver typed slot） |
| `r12_validation` | 红线扫描器的反向证明样本，请勿用于生产。 | -- |

## 多 Driver Module

单个 Module 可以通过 `src/api/cpp/module_base.h` 中声明的变参宏同时绑定到
多个传输驱动（例如 DDS + Iceoryx）：

```cpp
IBMW_USE_DRIVERS(
    IBMW_DRIVER_ENTRY(DdsDriverT, dds_driver_),
    IBMW_DRIVER_ENTRY(IceoryxDriverT, iceoryx_driver_))
```

宿主运行时在启动时通过 package ABI 把每个 driver 注入对应的 typed slot；
用户代码经各自声明的成员指针访问对应 driver，全程无字符串化查找。

完整可运行范例见 `src/samples/cpp/multi_driver_demo/`（DDS + Iceoryx，
含 managed / standalone / generated / pkg 四种模式）。

## 编辑 yaml 后建议运行的验证

```bash
# Pytest：把生成器输出锁定到 golden 文件。
cd src/tools/ibmw_gen && python3 -m pytest -q

# 树内 regen：每一份在树 yaml 都必须字节相等再生。
bash scripts/regen_in_tree.sh
```

## 当前限制

- **每个 group 仅支持单个 IDL 外部 target**：每个 DDS 类型 group 仅引用一个
  `idl_gencode_target`

> **关于零拷贝：** `loaned_publisher` / `loaned_subscriber` 在 DDS（FastDDS
> DataSharing，仅 Plain 类型）与 iceoryx（原生 SHM，仅 trivially-copyable
> 类型）下均已完整支持。端到端示例见
> `src/samples/cpp/loaned_message_demo/`。

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
