# `bag play` 冒烟测试

`play_smoke_test` 是一套端到端测试，用预先生成的 bag 验证 `ibmw bag play` 能否
把消息发布到配置的 transport，并验证常用的 play 命令行选项是否产生预期结果。
测试使用本目录独立维护的 YAML、bag 和测试 Module。

测试 subscriber 会使用对应的 CDR、JSON 或 ROS2 TypeSupport 反序列化消息，并把
每条消息写入本次运行的 artifact 目录。验证脚本检查 topic、消息数量、业务序号、
payload 顺序、transport driver（typed DDS/ROS2 路径则检查相应的 publisher/subscriber
注册日志）以及 player 的实际输出。全部检查通过且命令退出码为 0 时，用例通过。

## 构建

完整矩阵需要 Linux、DDS、MQTT、Zenoh、Net、ROS2、record/playback extension，
以及至少一种共享内存 transport：Iceoryx（`IBMW_BUILD_ICEORYX_EXTENSION`）或自研 shm
（`IBMW_BUILD_SHM_EXTENSION`）。构建了哪一种就运行哪一种的用例，两者都构建时两个用例
都运行。构建目标会在构建目录写入 `play_smoke_iceoryx_required` /
`play_smoke_shm_required` 标记，测试脚本据此决定运行哪些共享内存用例；只有构建了
Iceoryx 时才需要并启动 `iox-roudi`。shm 用例默认使用本次运行私有的 `IBMW_SHM_ROOT`
（运行结束后删除），调用方已设置 `IBMW_SHM_ROOT` 时沿用调用方的值。示例使用 `ros2` preset，并显式打开 Zenoh：

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake --preset ros2 -B build-play-smoke \
  -DIBMW_BUILD_ZENOH_EXTENSION=ON

cmake --build build-play-smoke \
  --target ibmw_samples_cpp_play_smoke_test_build_all \
  -j$(nproc)
```

构建目标会在 `build-play-smoke` 中生成：

- `validate_play_smoke.sh`：唯一的测试入口；
- `play_smoke_cfg/`：本测试自己维护的 subscriber、player 和 reader YAML；
- `play_smoke_bags/`：本测试自己生成的 MCAP/SQLite3 fixture；
- `libplay_smoke_pkg.so`、`libplay_smoke_ts.so` 和所需 transport 库；
- `play_smoke_ros2_runtime/`：ROS2 subscriber 所需的独立 ament 索引和 typesupport 库。

## 手动运行

只验证这套 play 测试时，在构建目录直接运行：

```bash
cd <IBMW_ROOT>/build-play-smoke
./validate_play_smoke.sh
```

也可以显式指定目录：

```bash
BUILD_DIR=<IBMW_ROOT>/build-play-smoke \
CFG_DIR=<IBMW_ROOT>/build-play-smoke/play_smoke_cfg \
BAGS_DIR=<IBMW_ROOT>/build-play-smoke/play_smoke_bags \
<IBMW_ROOT>/build-play-smoke/validate_play_smoke.sh
```

全部用例通过时输出：

```text
PASS: play smoke transport and CLI matrix
play smoke artifacts: /tmp/ibmw-play-smoke/run.XXXXXX
```

artifact 目录会保留每个用例的 player 日志、subscriber Runtime 日志、接收消息文件
和失败时的完整上下文。接收消息文件每行格式为：

```text
topic|sequence|payload|serialization|driver|receive_timestamp
```

## 纳入仓库总冒烟流程

纳入总冒烟流程时，先把构建产物收集到临时 stage 目录，再由统一入口调用
`validate_play_smoke.sh`：

```bash
cd <IBMW_ROOT>

BUILD="$PWD/build-play-smoke" \
STAGE=/tmp/ibmw-play-smoke-stage \
./scripts/stage_smoke.sh

cd /tmp/ibmw-play-smoke-stage
./run_all_tests.sh --ext-only
```

统一入口中的测试名称为 `bag_play_transport_and_cli_matrix`，最长运行 300 秒。
`--quick` 会跳过该测试。

## Transport 矩阵

每种 transport 都有自己的一对 player/subscriber YAML。player 是 `bag play` 在
运行时创建的 publisher，subscriber 是独立的 IBMW Runtime。
除了 local 的 Runtime route 外，下面的 external transport 用例都是：

```text
bag player publisher -> transport -> play-smoke subscriber
```

| Transport | fixture/type | 通过依据 |
|---|---|---|
| local | CDR MCAP | 收到 data `0..9`、secondary `100..109`，driver 为 `local` |
| DDS | CDR MCAP | native DDS subscriber 收到 10 条并按顺序解码；raw DDS publisher 注册成功；ACK 等待不超时 |
| Iceoryx（仅构建 Iceoryx 时） | CDR MCAP | 收到 10 条，序号/payload 正确，driver 为 `iceoryx` |
| shm（仅构建自研 shm 时） | CDR MCAP | 收到 10 条，序号/payload 正确，driver 为 `shm` |
| MQTT | CDR MCAP | 收到 10 条，序号/payload 正确，driver 为 `mqtt` |
| Zenoh | CDR MCAP | 收到 10 条，序号/payload 正确，driver 为 `zenoh` |
| ROS2 | ROS2 MCAP | ROS2 subscriber/publisher 注册成功，收到并解码 `ros2-0..ros2-9` |
| TCP | CDR MCAP | 至少收到完整一轮，首轮序号/payload 为 `0..9`，driver 为 `tcp` |
| UDP | CDR MCAP | 至少收到完整一轮，首轮序号/payload 为 `0..9`，driver 为 `udp` |
| HTTP | CDR MCAP | 至少收到完整一轮，首轮序号/payload 为 `0..9`，driver 为 `http` |

每个 external transport 还要求 player 确实创建 `Player-owned publisher`，并且
发布数量覆盖 subscriber 已验证的消息。TCP、UDP 和 HTTP 的发送是异步的，测试会
让 player 循环到 subscriber 收到完整首轮后再停止；停止前可能已经开始下一轮，
但首轮的 10 条消息仍必须逐条正确。

## CLI 功能矩阵

| 选项/能力 | 如何证明生效 |
|---|---|
| `--storage` | MCAP 与 SQLite3 都完整播放，且使用的 reader 与参数一致 |
| `--config-file` | 使用另一份独立 player YAML，日志显示选中的 route/runtime |
| `--topics` | 只收到 data，secondary 为 0 |
| `--remap` | 只在 remapped topic 收到 data，原 topic 为 0 |
| `--read-ahead-queue-size` | debug 输出报告 queue size 为 1，并完整收到消息 |
| `--rate` / `-r` | 1x 与 2x 都收到完整序列，2x 的接收时间跨度明显更短 |
| `--delay` / `-d` | 第一条消息相对启动时间至少延迟约 0.4 秒 |
| `--start-offset` | 从 fixture 的第 5 条开始，只收到序号 `4..9` |
| `--start-paused` | player 保持运行且暂停期间 0 条；发送 `SIGUSR2` 后收到完整序列 |
| `--disable-keyboard-controls` | PTY 中发送空格不会暂停，仍收到完整序列 |
| `--loop` / `-l` | 收到两轮 `0..9,0..9` 后用 SIGINT 停止 |
| `--storage-config-file` | MCAP 的 file-order fixture 必须收到 `0,2,1`；SQLite 在短暂独占锁后仍能在 YAML 的 busy timeout 内完整读取 |
| `--log-level` | info 显示 route 事件，error 隐藏该 info 事件但消息仍完整 |
| CDR → JSON transcode | JSON subscriber 收到同样的序号/payload，Context serialization 为 `json` |
| `--help` | play help 中列出所有正常 play 选项，包括 ACK、route、reader 和控制选项 |

`--wait-for-all-acked` 在 DDS transport 用例中额外执行，subscriber 收全 10 条且
等待 ACK 不超时。

## 文件说明

| 文件 | 用途 |
|---|---|
| `validate_play_smoke.sh` | 启动服务/Runtime，逐项运行 transport 与 CLI 用例并验证结果 |
| `generate_play_smoke_bags.cc` | 生成独立 CDR MCAP、CDR SQLite3 和 ROS2 MCAP fixture |
| `managed/play_smoke_subscriber_module.*` | 读取模块配置、注册 subscriber、记录接收消息 |
| `generated/play_smoke_subscriber.*` | CDR、JSON、ROS2 的反序列化适配 |
| `type_support_pkg_main.cc` | 提供 CDR/JSON/ROS2 TypeSupport 和 schema，支持 player 转码/发布 |
| `config/linux/bin/cfg/*.yaml` | 每个 transport、route 和 reader 的独立配置 |
