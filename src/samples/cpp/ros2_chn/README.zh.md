# ROS2 Transport 示例

## ROS2 Transport

一个使用 ROS2 msg 协议、local 后端的基础 transport 示例。本 demo 涵盖：
- 如何用 ROS2 msg 协议作为 transport 协议；
- 如何在 Module 模式下使用 Executor、Transport publish 与 subscribe；
- 如何使用 local transport 后端；
- 如何在 Pkg 模式下集成 Module 并启动；


核心代码：
- [example_ros2/msg/RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_publisher_module.cc](./managed/normal_publisher_module/normal_publisher_module.cc)
- [normal_subscriber_module.cc](./managed/normal_subscriber_module/normal_subscriber_module.cc)
- [ros2_chn_pub_pkg/pkg_main.cc](./package/ros2_chn_pub_pkg/pkg_main.cc)
- [ros2_chn_sub_pkg/pkg_main.cc](./package/ros2_chn_sub_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_chn_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_cfg.yaml)


如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_chn.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例创建以下两个 module：
  - `NormalPublisherModule`：使用 `work_thread_pool` executor，按配置频率向配置的 topic 发布 `RosTestMsg` 消息；
  - `NormalSubscriberModule`：订阅配置 topic 上的 `RosTestMsg` 消息；
- 本示例将 `NormalPublisherModule` 与 `NormalSubscriberModule` 分别集成到两个独立 package `ros2_chn_pub_pkg` 与 `ros2_chn_sub_pkg`，并通过配置文件加载到同一个 IBMW 进程；
- 本示例使用 local transport 后端通信；



## ROS2 Transport Single Pkg


一个使用 ROS2 msg 协议、local 后端的基础 transport 示例。本 demo 涵盖：
- 如何用 ROS2 msg 协议作为 transport 协议；
- 如何在 Module 模式下使用 Executor、Transport publish 与 subscribe；
- 如何使用 local transport 后端；
- 如何在 Pkg 模式下集成 Module 并启动；


核心代码：
- [example_ros2/msg/RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_publisher_module.cc](./managed/normal_publisher_module/normal_publisher_module.cc)
- [normal_subscriber_module.cc](./managed/normal_subscriber_module/normal_subscriber_module.cc)
- [ros2_chn_pkg/pkg_main.cc](./package/ros2_chn_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_chn_single_pkg_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_single_pkg_cfg.yaml)


如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_chn_single_pkg.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 本示例本质上与 **ROS2 Transport** 相同，唯一区别是 `NormalPublisherModule` 与 `NormalSubscriberModule` 被集成到同一个 package `ros2_chn_pkg`；

## ROS2 Transport Publisher App


一个使用 ROS2 msg 协议、local 后端的 transport 示例。本 demo 涵盖：
- 如何用 ROS2 msg 协议作为 transport 协议；
- 如何在 App 模式下创建 module 并使用 Transport publish 功能；
- 如何在 Module 模式下使用 Transport subscribe 功能；
- 如何使用 local transport 后端；
- 如何以 App 模式启动；


核心代码：
- [RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_ros2_chn_publisher_app/main.cc](./standalone/normal_ros2_chn_publisher_app/main.cc)
- [normal_subscriber_module.cc](./managed/normal_subscriber_module/normal_subscriber_module.cc)
- [ros2_chn_pub_pkg/pkg_main.cc](./package/ros2_chn_pub_pkg/pkg_main.cc)


配置文件：
- [examples_cpp_ros2_chn_publisher_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_publisher_app_cfg.yaml)


如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_chn_publisher_app.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 在 App 模式下，本示例直接创建 `NormalPublisherModule`，获取 CoreRef 句柄，按配置频率向配置 topic 发布 `RosTestMsg` 消息；
- 本示例使用 local transport 后端通信；
- 本示例以 App 模式启动；


## ROS2 Transport Subscriber App


一个使用 ROS2 msg 协议、local 后端的 transport 示例。本 demo 涵盖：
- 如何用 ROS2 msg 协议作为 transport 协议；
- 如何在 App 模式下创建 module 并使用 Transport subscribe 功能；
- 如何使用 local transport 后端；
- 如何以 App 模式启动；


核心代码：
- [RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_ros2_chn_subscriber_app/main.cc](./standalone/normal_ros2_chn_subscriber_app/main.cc)
- [normal_publisher_module.cc](./managed/normal_publisher_module/normal_publisher_module.cc)
- [ros2_chn_pub_pkg/pkg_main.cc](./package/ros2_chn_pub_pkg/pkg_main.cc)



配置文件：
- [examples_cpp_ros2_chn_subscriber_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_subscriber_app_cfg.yaml)


如何运行（Linux）：
- 启用 `IBMW_BUILD_EXAMPLES` 与 `IBMW_BUILD_WITH_ROS2` 构建 IBMW；
- 在构建目录下运行 `start_examples_cpp_ros2_chn_subscriber_app.sh` 启动进程；
- 按 `ctrl-c` 停止进程；


说明：
- 在 App 模式下，本示例直接创建 `NormalSubscriberModule`，获取 CoreRef 句柄，订阅配置 topic 上的 `RosTestMsg` 消息；
- 本示例使用 local transport 后端通信；
- 本示例以 App 模式启动；

---

## ibmw-gen 方式

本 demo 也可以通过 `ibmw-gen` 从
[`ros2_chn.ibmw.yaml`](./ros2_chn.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 生成（自动） | 你自己写 |
|-------------------|-----------|
| `CMakeLists.txt` —— 带 ROS2 package 链接的构建系统 | `normal_publisher_module.cc` —— publish 逻辑 |
| `cfg/ros2_chn_app.yaml` —— App 模式配置 | `normal_subscriber_module.cc` —— subscribe 回调 |
| `cfg/ros2_chn_pkg.yaml` —— Pkg 模式配置 | Module `.h` 头文件 |
| `src/ros2_chn_app/main.cc` —— App 入口 | |
| `src/*_pkg/pkg_main.cc` —— Pkg 加载器 | |

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py ros2_chn.ibmw.yaml -o ros2_chn_gen

# 构建
source /opt/ros/humble/setup.bash
mkdir -p ros2_chn_gen/build && cd ros2_chn_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行（App 模式）
./ros2_chn_app cfg/ros2_chn_app.yaml

# 运行（Pkg 模式）
ibmw run cfg/ros2_chn_pkg.yaml
```

### 关键 YAML 特性

```yaml
modules:
  - name: NormalPublisherModule
    type: publisher
    transport: ros2                                 # ROS2 transport 后端
    topic: test_topic
    message_type: "example_ros2::msg::RosTestMsg"
    ros2_msg_include: "example_ros2/msg/ros_test_msg.hpp"  # ROS2 消息头文件
    ros2_package: example_ros2                             # ROS2 package 依赖
```

ibmw-gen 生成 `SenderAccess<example_ros2::msg::RosTestMsg>` 封装以及
CMake `find_package(example_ros2)` 集成。Module 源文件中包含实际 publish
timer 与 subscribe 回调逻辑的 TODO 标记。

**注意：** benchmark 的 publisher/subscriber module 未包含在 ibmw-gen 版本中，
因为它们需要自定义 config 解析、动态 executor 池以及多 topic benchmark 逻辑，
超出了当前 generator 范围。
