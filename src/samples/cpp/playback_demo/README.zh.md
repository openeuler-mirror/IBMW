# Playback 示例

一个使用预置 MCAP 和 SQLite3 bag 验证 IBMW playback 的示例。本 demo 涵盖：

- 使用 `ibmw bag info` 检查两种存储格式；
- 使用 `ibmw bag play` 读取预置 bag；
- 通过配置将播放数据发布到 DDS；
- 在独立 IBMW Runtime 的 subscriber Module 中打印收到的话题数据。

本示例不需要先运行 publisher 或重新录制。仓库中保存的两份 bag 包含相同的 5 条
`ibmw::protocols::common::Header` 消息，topic 均为 `playback_demo_topic`，serialization
type 均为 `cdr`。

## 核心代码

- [subscriber_module.h](./managed/subscriber_module/subscriber_module.h)
- [subscriber_module.cc](./managed/subscriber_module/subscriber_module.cc)
- [playback_demo_subscriber.h](./generated/playback_demo_subscriber.h)
- [playback_demo_subscriber.cc](./generated/playback_demo_subscriber.cc)
- [pkg_main.cc](./package/playback_demo_pkg/pkg_main.cc)

## 配置和 Bag

- [playback_demo_subscriber.yaml](./config/linux/bin/cfg/playback_demo_subscriber.yaml)
- [playback_demo_player.yaml](./config/linux/bin/cfg/playback_demo_player.yaml)
- [MCAP bag](./bags/mcap/metadata.yaml)
- [SQLite3 bag](./bags/sqlite3/metadata.yaml)

`playback_demo_subscriber.yaml` 启动 DDS subscriber 和
`PlaybackSubscriberModule`。`playback_demo_player.yaml` 由 `ibmw bag play` 使用，负责
创建内部 DDS publisher；它不会启动 subscriber Module。

## 依赖

项目的通用编译环境和依赖安装方法见仓库根目录的 `README.zh.md`。本 demo 只需要
DDS、protocols 和 record/playback extension，不需要启动 RouDi、MQTT Broker 或
ROS2 节点。

## 构建

```bash
cd <IBMW_ROOT>
cmake --preset minimal -B build-playback-demo \
  -DCMAKE_BUILD_TYPE=Release \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_PROTOCOLS=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON
cmake --build build-playback-demo \
  --target ibmw_samples_cpp_playback_demo_build_all \
  -j$(nproc)
```

`minimal` preset 提供 Runtime 的基础配置，后面的选项只打开本 demo 需要的 Examples、
Protocols、DDS 和 record/playback extension，不要求 ROS2、Iceoryx 或 MQTT。聚合目标会
重新构建 `ibmw`、DDS extension、record/playback extension、TypeSupport 和 subscriber
package，并把配置、脚本及预置 bag 复制到构建目录。运行过程不依赖其他 build 目录中
的产物。

## 运行

1. 在终端一启动 subscriber：

```bash
cd <IBMW_ROOT>/build-playback-demo
bash ./start_playback_subscriber.sh
```

保持该终端运行。DDS 使用 `volatile` durability，因此 subscriber 应在播放前启动；
播放结束后才启动的 subscriber 不会收到已经发出的历史消息。

2. 在终端二查看并播放 MCAP：

```bash
cd <IBMW_ROOT>/build-playback-demo

./ibmw bag info ./playback_demo_bags/mcap
./ibmw bag play ./playback_demo_bags/mcap \
  --storage mcap \
  --config-file ./cfg/playback_demo_player.yaml
```

3. 继续在终端二查看并播放 SQLite3：

```bash
./ibmw bag info ./playback_demo_bags/sqlite3
./ibmw bag play ./playback_demo_bags/sqlite3 \
  --storage sqlite3 \
  --config-file ./cfg/playback_demo_player.yaml
```

每次播放完成后，终端二应输出：

```text
Published 5 message(s).
```

终端一应分别收到 5 条消息，并打印类似：

```text
Received playback message #1: sequence=16, frame_id='record_demo', timestamp=...
Received playback message #2: sequence=17, frame_id='record_demo', timestamp=...
```

两种 bag 都验证完成后，在终端一按 `Ctrl+C` 停止 subscriber。

## 数据路径

播放路径如下：

```text
MCAP/SQLite3 bag
  -> ibmw bag play
  -> player YAML 创建的 DDS publisher
  -> DDS subscriber Runtime
  -> PlaybackSubscriberModule 回调
  -> CDR 反序列化后的 Header 对象
```

MCAP 和 SQLite3 只是落盘格式；实际发布 transport 由
`--config-file ./cfg/playback_demo_player.yaml` 决定。
