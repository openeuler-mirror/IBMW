# ROS2 Point Demo

演示用 IBMW DDS 插件对 ROS2 plain 类型（`geometry_msgs::msg::Point`）做零拷贝通信。

## 依赖

- ROS2 Humble（`source /opt/ros/humble/setup.bash`）
- geometry_msgs 消息包

## 构建

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake -B build \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_WITH_ROS2=ON

cmake --build build --target ros2_point_app
```

## 运行

```bash
source /opt/ros/humble/setup.bash
cd build
./ros2_point_app ../src/samples/cpp/ros2_point_demo/config/cfg/ros2_point_cfg.yaml
```

## 零拷贝细节

`geometry_msgs::msg::Point` 是 plain 类型（`is_trivially_copyable` +
`is_standard_layout`），插件自动识别并启用零拷贝优化：

- Publisher：`loan_sample` + `memcpy`（1 拷贝）
- Subscriber：`TakeLoan`（0 拷贝）

## ROS2 互操作

```bash
# 终端 1：IBMW 发布
./ros2_point_app ../src/samples/cpp/ros2_point_demo/config/cfg/ros2_point_cfg.yaml

# 终端 2：ROS2 订阅
ros2 topic echo /rt/point_topic geometry_msgs/msg/Point
```

## 配置

参见 `config/cfg/ros2_point_cfg.yaml`。

---

## ibmw-gen 方式

本 Demo 也可由 `ibmw-gen` 从
[`ros2_point_demo.ibmw.yaml`](./ros2_point_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 自动生成 | 你来写 |
|----------|--------|
| `CMakeLists.txt` —— 含 geometry_msgs + ros2_types_impl 链接 | `point_publisher_module.cc` —— Point 发布逻辑 |
| `cfg/ros2_point_app.yaml` —— 含 DDS ROS2 兼容模式的 App 配置 | `point_subscriber_module.cc` —— Point 订阅回调 |
| `cfg/ros2_point_pkg.yaml` —— Pkg 模式配置 | Module `.h` 头文件 |
| `src/ros2_point_app/main.cc`、`src/*_pkg/pkg_main.cc` | |

### 关键 YAML 字段

```yaml
dds_driver:
  domain_id: 0
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"

modules:
  - name: PointPublisherModule
    type: ros2_interop_publisher
    topic: "point_topic"
    message_type: "geometry_msgs::msg::Point"
    ros2_msg_include: "geometry_msgs/msg/point.hpp"
    ros2_type_name: "geometry_msgs/msg/Point"
    ros2_packages: [geometry_msgs]       # 额外的 ROS2 包依赖
```

`ros2_packages` 字段告诉 ibmw-gen 在生成的 CMakeLists.txt 里加上
`find_package(geometry_msgs)` 并链接 `${geometry_msgs_TARGETS}`。`std_msgs` 已经
是 `ibmw::extensions::ros2_types` 的传递依赖，无需再列。
