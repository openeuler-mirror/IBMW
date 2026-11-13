# Record 示例

一个同时使用 DDS、共享内存（Iceoryx 和/或自研 shm）、MQTT 和 ROS2 transport 的
record 示例。本 demo 涵盖：

- 如何在多个独立的 IBMW Runtime 中分别启动 publisher（每个 transport 一个）；
- 如何使用 `ibmw bag record` 同时录制各 transport 的 topic；
- 如何使用 `ibmw bag info` 查看 topic、消息数量、序列化类型和 bag 完整性；
- 如何在同一个 bag 中保存 `cdr` 与 `ros2` 两种 serialization type。

本示例只验证 `record` 和 `info`，不额外创建 subscriber Module。录制器根据 Runtime
Agent 发布的 topic advertisement 创建对应的 capture source。


## 核心代码

- [cdr_publisher_module.h](./managed/cdr_publisher_module/cdr_publisher_module.h)
- [cdr_publisher_module.cc](./managed/cdr_publisher_module/cdr_publisher_module.cc)
- [ros2_publisher_module.h](./managed/ros2_publisher_module/ros2_publisher_module.h)
- [ros2_publisher_module.cc](./managed/ros2_publisher_module/ros2_publisher_module.cc)
- [record_demo_publishers.h](./generated/record_demo_publishers.h)
- [record_demo_publishers.cc](./generated/record_demo_publishers.cc)
- [record_demo_ros2_publisher.cc](./generated/record_demo_ros2_publisher.cc)
- [pkg_main.cc](./package/record_demo_pkg/pkg_main.cc)


## 配置文件

- [record_demo_dds.yaml](./config/linux/bin/cfg/record_demo_dds.yaml)
- [record_demo_iceoryx.yaml](./config/linux/bin/cfg/record_demo_iceoryx.yaml)（仅 `IBMW_BUILD_ICEORYX_EXTENSION=ON` 时启动）
- [record_demo_shm.yaml](./config/linux/bin/cfg/record_demo_shm.yaml)（仅 `IBMW_BUILD_SHM_EXTENSION=ON` 时启动）
- [record_demo_mqtt.yaml](./config/linux/bin/cfg/record_demo_mqtt.yaml)
- [record_demo_ros2.yaml](./config/linux/bin/cfg/record_demo_ros2.yaml)
- [record_demo_env.sh](./config/linux/bin/record_demo_env.sh)
- [start_record_demo.sh](./config/linux/bin/start_record_demo.sh)


## 消息与 Transport

| Runtime | Module | transport | topic | serialization |
|----------|--------|-----------|-------|---------------|
| DDS | `CdrPublisherModule` | DDS | `record_demo_dds_topic` | `cdr` |
| Iceoryx | `CdrPublisherModule` | Iceoryx | `record_demo_iceoryx_topic` | `cdr` |
| shm（自研） | `CdrPublisherModule` | shm | `record_demo_shm_topic` | `cdr` |
| MQTT | `CdrPublisherModule` | MQTT | `record_demo_mqtt_topic` | `cdr` |
| ROS2 | `Ros2PublisherModule` | ROS2 | `record_demo_ros2_topic` | `ros2` |

DDS、Iceoryx、shm 和 MQTT 使用 `ibmw::protocols::common::Header` 的 CDR TypeSupport；
ROS2 使用 `example_ros2::msg::RosTestMsg`。因此 recorder 命令使用
`--serialization-format auto`，保留每个 topic 的原始格式。


## 依赖

- ROS2 Humble（构建前执行 `source /opt/ros/humble/setup.bash`）；
- Mosquitto MQTT Broker（运行 MQTT publisher 时使用）。

项目的通用编译环境和依赖安装方法见仓库根目录的 `README.zh.md`。

## 构建

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash
cmake --preset ros2 -B build-record-demo
cmake --build build-record-demo \
  --target ibmw_samples_cpp_record_demo_build_all \
  -j$(nproc)
```

`ros2` preset 会构建本 demo 使用的 transport、record/playback extension、
TypeSupport、Module package 和 `iox-roudi`。本 demo 的构建门禁为 DDS + MQTT + ROS2 +
record/playback，且 `IBMW_BUILD_ICEORYX_EXTENSION` 与 `IBMW_BUILD_SHM_EXTENSION`
至少开启一个；只开 shm（`-DIBMW_BUILD_ICEORYX_EXTENSION=OFF
-DIBMW_BUILD_SHM_EXTENSION=ON`）时不构建也不启动 Iceoryx publisher 和 `iox-roudi`。
CMake 在构建目录生成 `record_demo_transports.sh`，记录本次构建包含哪些共享内存
transport，启动脚本据此决定启动哪些 Runtime。聚合目标还会在构建目录中生成
`record_demo_ros2_runtime` 私有前缀，供 ROS2 transport 查找 `example_ros2` 类型；
不需要执行全工程的 `cmake --install`。

## 运行

1. 在终端一启动 publisher：

```bash
cd <IBMW_ROOT>/build-record-demo
source ./record_demo_env.sh
bash ./start_record_demo.sh
```

脚本会启动：
- `./iox-roudi`：本次源码构建产生的 Iceoryx daemon（仅构建了 Iceoryx 时）；
- `mosquitto -p 1884`：本地 MQTT Broker；
- 每个 transport 一个 `ibmw run` publisher Runtime：DDS、Iceoryx（若构建）、
  shm（若构建）、MQTT、ROS2。

看到 `<N> record_demo publishers are running.` 后保持终端一运行（两种共享内存
transport 都构建时 N 为 5，只有其中一种时为 4），脚本同时打印需要录制的 topic 列表。
各 Runtime 的日志写入脚本新建的私有目录 `${TMPDIR:-/tmp}/ibmw-record-demo.XXXXXX/record_demo_*.log`
（启动时会打印实际路径；设置 `RECORD_DEMO_LOG_DIR` 可指定目录）。启动脚本不会依赖旧构建产物：`ibmw`、
`iox-roudi`、`librecord_demo_pkg.so`、transport plugin、TypeSupport、YAML 和脚本均由
前面的 fresh build 生成；系统只需提供 ROS2 运行库及 `mosquitto`。shm publisher 不需要
daemon；recorder 与它必须解析到同一个 shm 根目录和 domain（`$IBMW_SHM_ROOT` /
`$IBMW_SHM_DOMAIN`，未设置时为 `/dev/shm`（不可用时为 `/tmp`）和 `default`），因此两个终端的这两个
环境变量要保持一致。

2. 在终端二录制各 topic（下例为两种共享内存 transport 都构建的情况；未构建的
transport 对应的 topic 去掉即可）：

```bash
cd <IBMW_ROOT>/build-record-demo
source ./record_demo_env.sh

./ibmw bag record \
  record_demo_dds_topic \
  record_demo_iceoryx_topic \
  record_demo_shm_topic \
  record_demo_mqtt_topic \
  record_demo_ros2_topic \
  --serialization-format auto \
  -o record_demo_bag
```

让 recorder 运行几秒后按 `Ctrl+C` 结束录制。录制期间终端一的 publisher
必须保持运行；录制结束后 publisher 仍可以继续运行。

3. 在终端二查看录制结果：

```bash
./ibmw bag info record_demo_bag
```


说明：
- 两个 managed Module 只使用统一 publisher 接口；框架所有的 `generated` 适配层
  负责注册并发送 `Header`/`RosTestMsg`，具体 TypeSupport 不进入用户可编辑代码；
- `CdrPublisherModule` 具体使用 DDS、Iceoryx、shm 还是 MQTT，由各自 Runtime 中唯一启用的
  driver 决定；`Ros2PublisherModule` 的 IBMW serialization type 是 `ros2`；
- 每个 publisher Runtime 都启用了 record/playback extension 的 Runtime Agent，
  recorder 通过 `$XDG_RUNTIME_DIR/ibmw/v2`（否则 `/tmp/ibmw-<euid>/v2`）中的 advertisement 发现它们；
- `record_demo_env.sh` 在发布端和 recorder 端都要 source，因为两个终端是独立的
  shell 环境；
- `--serialization-format auto` 会保留每个 topic 的原始 serialization type。不要
  改成 `cdr`，因为本示例的 ROS2 topic 声明为 `ros2`；
- 本 demo 使用项目中保留的 rcl-based `ros2_extension`，目的是显式验证 `ros2`
  serialization channel。新的 ROS2 跨进程互操作场景通常使用 DDS transport 的
  `ros2_compatible` 模式。


预期输出（`bag info`）：

每个 topic 的 `Messages` 都应大于 0，ROS2 topic 的 serialization 为 `ros2`，其余
topic 为 `cdr`。正常录制还应看到：

```text
Topic information:
  Topic: record_demo_dds_topic
    Serialization: cdr
  Topic: record_demo_iceoryx_topic
    Serialization: cdr
  Topic: record_demo_shm_topic
    Serialization: cdr
  Topic: record_demo_mqtt_topic
    Serialization: cdr
  Topic: record_demo_ros2_topic
    Serialization: ros2

Closed cleanly:    yes
Dropped messages:  0
Complete playback: yes
```

各 publisher 默认按 1 Hz 发布，消息数量取决于录制时长。检查完成后，回到终端一
按 `Ctrl+C`，启动脚本会停止它启动的 Runtime 以及 RouDi/Broker。
