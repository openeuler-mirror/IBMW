# ROS2-Ibmw Interop Demo

本 demo 演示 ROS2-Ibmw 互操作，采用**解耦架构**，不依赖 RCL/RMW 层。

## 特性

- ✅ **不依赖 RCL/RMW** —— 直接使用 FastDDS API
- ✅ **标准 CDR 格式** —— 与 ROS2 兼容
- ✅ **Ibmw 回调执行** —— 使用 Ibmw 的 executor 机制
- ✅ **ROS2 CLI 工具** —— 可用 `ros2 topic list/echo/info`
- ✅ **零拷贝支持** —— 针对 Plain 类型（Point、Pose 等）

## 架构

```
Ibmw Application
  ├─ Publisher Module（发布 std_msgs/msg/String）
  ├─ Subscriber Module（订阅 std_msgs/msg/String）
  └─ DDS Extension（ROS2 兼容模式）
       ↓
    FastDDS（标准 DDS）
       ↓
    ROS2 rmw_fastrtps Layer
       ↓
    ROS2 Application（talker/listener）
```

## 构建

```bash
cd <IBMW_ROOT>/build

# Source ROS2 环境
source /opt/ros/humble/setup.bash

# 启用 ROS2 支持配置
cmake .. -DIBMW_BUILD_EXAMPLES=ON -DIBMW_BUILD_DDS_EXTENSION=ON

# 构建
cmake --build . --target ros2_interop_app --parallel $(nproc)
```

## 运行

### 测试 1：Ibmw Subscriber + ROS2 Talker

```bash
cd <IBMW_ROOT>/build/ros2_interop_demo

# 给脚本加可执行权限
chmod +x scripts/test_with_ros2_talker.sh

# 运行测试
./scripts/test_with_ros2_talker.sh
```

**预期输出：**
- ROS2 Talker 发布："Hello World: 0"、"Hello World: 1" ……
- Ibmw Subscriber 接收并打印消息

### 测试 2：Ibmw Publisher + ROS2 Listener

```bash
cd <IBMW_ROOT>/build/ros2_interop_demo

# 给脚本加可执行权限
chmod +x scripts/test_with_ros2_listener.sh

# 运行测试
./scripts/test_with_ros2_listener.sh
```

**预期输出：**
- Ibmw Publisher 发布："Hello from Ibmw: 0"、"Hello from Ibmw: 1" ……
- ROS2 Listener 接收并打印消息

### 测试 3：双向通信

```bash
cd <IBMW_ROOT>/build/ros2_interop_demo

# 终端 1：启动 Ibmw（同时含 publisher 与 subscriber）
./ros2_interop_app ./cfg/ros2_interop_cfg.yaml

# 终端 2：启动 ROS2 Talker
ros2 run demo_nodes_cpp talker

# 终端 3：启动 ROS2 Listener
ros2 run demo_nodes_cpp listener
```

**预期输出：**
- Ibmw 接收 ROS2 Talker 的消息
- ROS2 Listener 接收 Ibmw Publisher 的消息

## 用 ROS2 CLI 工具验证

```bash
# 1. 列出 topic
ros2 topic list
# 输出：/chatter

# 2. 查看 topic 信息
ros2 topic info /chatter
# 输出：
# Type: std_msgs/msg/String
# Publisher count: 1
# Subscription count: 1

# 3. 回显消息
ros2 topic echo /chatter
# 输出：data: "Hello from Ibmw: 0"

# 4. 查看消息频率
ros2 topic hz /chatter
# 输出：average rate: 1.000

# 5. 查看带宽
ros2 topic bw /chatter
# 输出：average: 0.05KB/s
```

## 配置

本 demo 使用 `cfg/ros2_interop_cfg.yaml`：

```yaml
ibmw:
  extensions:
    items:
      - name: dds_extension
        options:
          domain_id: 0
          ros2_compatible_mode: true  # 启用 ROS2 兼容
          ros2_topic_prefix: "rt"     # ROS2 topic 前缀

  messaging:
    drivers:
      - type: dds
        options:
          pub_topics_options:
            - topic: "(.*)"
              qos:
                history: keep_last
                depth: 10
                reliability: reliable
                durability: volatile
```

**关键设置：**
- `ros2_compatible_mode: true` —— 启用 ROS2 topic 命名（`rt/chatter`）
- `ros2_topic_prefix: "rt"` —— ROS2 标准前缀
- QoS 与 ROS2 默认值匹配

## 故障排查

### 问题：`ros2 topic list` 看不到 topic

**解决：**
1. 检查 ROS_DOMAIN_ID：`echo $ROS_DOMAIN_ID`（应为 0 或未设置）
2. 验证 DDS discovery：`fastdds discovery`
3. 检查配置：`ros2_compatible_mode: true`

### 问题：消息收不到

**解决：**
1. 检查 topic 名：Ibmw 用 `/chatter`，DDS 用 `rt/chatter`
2. 检查 QoS 兼容性（Reliability、Durability）
3. 验证类型名：`std_msgs::msg::dds_::String_`

### 问题：构建错误

**解决：**
```bash
# Source ROS2 环境
source /opt/ros/humble/setup.bash

# 检查 ROS2 包
ros2 pkg list | grep std_msgs

# 重新配置 CMake
cd build
rm -rf *
cmake .. -DIBMW_BUILD_EXAMPLES=ON -DIBMW_BUILD_DDS_EXTENSION=ON
```

## 性能

**预期性能：**
- 延迟：< 200 μs（同主机）
- 吞吐：> 1000 Hz
- 零拷贝：< 100 μs（Plain 类型）

**Benchmark：**
```bash
# 高频测试（将发布频率改成 1000 Hz）
ros2 topic hz /chatter
# 预期：~1000 Hz
```

## 实现细节

### 解耦架构

**不使用：**
- ❌ `rclcpp` —— ROS2 客户端库
- ❌ `rmw_fastrtps_cpp` —— RMW 实现
- ❌ `rosidl_typesupport_cpp` —— TypeSupport 层

**使用：**
- ✅ `rosidl_generator_cpp` —— 仅消息定义
- ✅ `fastdds` —— 直接调用 FastDDS API
- ✅ `fastcdr` —— 直接 CDR 序列化
- ✅ Ibmw executor —— 自己的回调机制

### 消息流

```
Publisher：
  std_msgs::msg::String
    ↓ (Ibmw API)
  ROS2CdrSerializer::Serialize()
    ↓ (FastCDR)
  CDR bytes
    ↓ (FastDDS DataWriter)
  DDS Network

Subscriber：
  DDS Network
    ↓ (FastDDS DataReader)
  CDR bytes
    ↓ (FastCDR)
  ROS2CdrSerializer::Deserialize()
    ↓ (Ibmw callback)
  std_msgs::msg::String
```

### 类型注册

```cpp
// 注册 ROS2 类型（无 RMW 依赖）
ibmw::ros2::transport::RegisterPublishType<std_msgs::msg::String>(
  publisher, "std_msgs/msg/String");

// 发布（使用 Ibmw 回调）
std_msgs::msg::String msg;
msg.data = "Hello";
ibmw::ros2::transport::Send(publisher, "std_msgs/msg/String", msg);

// 订阅（使用 Ibmw 回调）
ibmw::ros2::transport::Subscribe<std_msgs::msg::String>(
  subscriber, "std_msgs/msg/String",
  [](auto ctx, const auto& msg) {
    // Ibmw executor 处理这个回调
    std::cout << msg->data << std::endl;
  });
```

## 已支持的消息类型

当前支持：
- `std_msgs/msg/String`
- `std_msgs/msg/Int32`、`Int64`
- `std_msgs/msg/Float32`、`Float64`
- `std_msgs/msg/Bool`
- `geometry_msgs/msg/Point`
- `geometry_msgs/msg/Quaternion`
- `geometry_msgs/msg/Pose`
- `geometry_msgs/msg/Twist`
- `geometry_msgs/msg/Vector3`

如需新增类型，参见 `src/interface/ibmw_module_ros2_interface/util/ros2_cdr_serializer_impl.h`

## 参考

- 设计文档：`ROS2_IBMW_DECOUPLED_DESIGN_PART*.md`
- 实现进度：`ROS2_IBMW_IMPLEMENTATION_PROGRESS.md`
- ROS2 分析：`analysis_ros_type/ROS2_消息处理与DDS集成分析.md`

---

## ibmw-gen 方式

本 demo 也可以通过 `ibmw-gen` 从
[`ros2_interop_demo.ibmw.yaml`](./ros2_interop_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 生成（自动） | 你自己写 |
|-------------------|-----------|
| `CMakeLists.txt` —— 含 ros2_types_impl + whole-archive 链接的构建系统 | `publisher_module.cc` —— ROS2 publish 逻辑（RegisterSendType + Send） |
| `cfg/ros2_interop_app.yaml` —— pub+sub 合并配置（含 DDS ROS2 兼容） | `subscriber_module.cc` —— ROS2 subscribe 回调（OnMessage） |
| `cfg/ros2_pub_only_app.yaml`、`cfg/ros2_sub_only_app.yaml` | Module `.h` 头文件 |
| `cfg/ros2_interop_pkg.yaml` —— Pkg 模式配置 | |
| `src/ros2_interop_app/main.cc`、`src/*_pkg/pkg_main.cc` | |

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py ros2_interop_demo.ibmw.yaml -o ros2_interop_demo_gen

# 构建
source /opt/ros/humble/setup.bash
mkdir -p ros2_interop_demo_gen/build && cd ros2_interop_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行（App 模式 —— pub+sub 合并）
./ros2_interop_app cfg/ros2_interop_app.yaml

# 运行（Pkg 模式）
ibmw run cfg/ros2_interop_pkg.yaml

# 运行（仅 pub 或仅 sub）
./ros2_pub_only_app cfg/ros2_pub_only_app.yaml
./ros2_sub_only_app cfg/ros2_sub_only_app.yaml
```

### 关键 YAML 特性

```yaml
dds_driver:
  domain_id: 0
  ros2_compatible_mode: true     # 启用 ROS2 topic 命名
  ros2_topic_prefix: "rt"        # ROS2 标准前缀

modules:
  - name: PublisherModule
    type: ros2_interop_publisher  # 低层 ROS2 DDS publisher（无 RCL）
    topic: "chatter"
    message_type: "std_msgs::msg::String"
    ros2_msg_include: "std_msgs/msg/string.hpp"
    ros2_type_name: "std_msgs/msg/String"
```

`ros2_interop_publisher/subscriber` module 类型使用底层
`ibmw::ros2::transport::*` API（`RegisterSendType`、`Send`、`OnMessage`），
而不是更高层的 `SenderAccess`/`ReceiverAccess` 封装。这样可在不依赖 RCL/RMW
的前提下，与 ROS2 节点直接做 DDS 线协议级别的兼容。

ibmw-gen 生成内容包括：
- 针对 `ibmw_extensions_ros2_types_impl` 的 CMake `whole-archive` 链接
- 含 `ros2_compatible_mode` 与 `ros2_topic_prefix` 的 DDS extension 配置
- 带正确 `#include` 与 `RegisterSendType`/`OnMessage` 模式的 module 脚手架
