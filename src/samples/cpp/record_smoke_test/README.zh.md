# Record 冒烟测试

`record_smoke_test` 用于验证 `ibmw bag record` 能否通过各类 transport 录制
消息，以及 record CLI 的主要功能是否可用。

测试脚本会自动完成以下工作：

1. 启动 Iceoryx RouDi 和 MQTT Broker；
2. 根据独立 YAML 启动测试 Runtime；
3. 启动 `ibmw bag record` 并录制消息；
4. 停止 recorder 和 Runtime；
5. 通过 `ibmw bag info` 检查录制结果。

测试范围是 `record`；生成的 bag 通过 `bag info` 检查。


## 构建

该测试需要同时启用 DDS、Iceoryx、MQTT、Zenoh、ROS2、Net 和
record/playback extension。`ros2` preset 已包含除 Zenoh 外的所需选项：

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake --preset ros2 -B build-record-smoke \
  -DIBMW_BUILD_ZENOH_EXTENSION=ON

cmake --build build-record-smoke \
  --target ibmw_samples_cpp_record_smoke_test_build_all \
  -j$(nproc)
```

构建完成后，`build-record-smoke` 中会包含测试脚本、测试 YAML、测试 Module、
TypeSupport 和所需运行库，不需要执行 `cmake --install`。


## 运行测试

这套测试有两种运行方式：手动运行 record 测试，或者通过仓库的总冒烟测试流程
运行。两种方式最终执行的都是同一个 `validate_record_smoke.sh`。


### 方式一：手动运行 record 测试

开发和调试本测试时使用这种方式：

```bash
cd <IBMW_ROOT>/build-record-smoke
./validate_record_smoke.sh
```

脚本默认把自身所在目录作为构建目录，并从该目录的 `cfg` 子目录加载配置，不需要
额外设置 `BUILD_DIR` 或 `CFG_DIR`。

脚本会依次执行 transport 矩阵和 CLI 功能矩阵。每个用例独立执行，全部用例完成后
汇总结果。全部通过时输出：

```text
PASS: record smoke transport and CLI matrix
record smoke artifacts: /tmp/ibmw-record-smoke/run.XXXXXX
```

输出中的 artifact 目录保存本次运行产生的 bag，以及 Runtime、recorder 和
`bag info` 日志。测试失败时，脚本会打印失败用例和对应日志，并最终返回非 0。


### 方式二：通过总冒烟测试流程运行

仓库的总冒烟流程先收集测试所需文件，再从统一入口执行所有 extension 测试：

```bash
cd <IBMW_ROOT>

BUILD="$PWD/build-record-smoke" \
STAGE=/tmp/ibmw-record-smoke-stage \
./scripts/stage_smoke.sh

cd /tmp/ibmw-record-smoke-stage
./run_all_tests.sh --ext-only
```

总流程分为准备和执行两个步骤：

- `stage_smoke.sh` 只准备运行目录，不执行测试。它把 `ibmw`、动态库、测试 YAML、
  `validate_record_smoke.sh` 和 `record_smoke_ros2_runtime` 收集到
  `/tmp/ibmw-record-smoke-stage`；
- `run_all_tests.sh --ext-only` 执行仓库中的 extension 冒烟测试，并在其中调用
  `validate_record_smoke.sh`。

`stage_smoke.sh` 完成准备后，统一入口执行与手动运行相同的 record 测试用例。
只运行 record 测试时，使用方式一即可。

统一入口中的测试名称为 `bag_record_transport_and_cli_matrix`，最长运行 300 秒。
`--quick` 会跳过该测试，执行完整 record 总回归时使用不带 `--quick` 的命令。


## Transport 覆盖

每种 transport 使用自己的 YAML、Runtime ID 和 topic。测试先启动使用该 transport
发布消息的 Runtime，再启动 `bag record --registry`，按驱动采集能力独立订阅或使用 Runtime Tap 录制这个 topic。
这些用例验证本机发布采集；DDS、ROS 2、Zenoh 原生订阅由各传输模块的回归测试覆盖。通过标准是：bag 中出现预期
的 topic，至少包含 5 条消息；`ibmw bag info` 能正常读取；没有 dropped/duplicate
message；recorder 收到停止信号后以退出码 0 完成关闭。以上结果共同确认该
transport 发布的消息已由 recorder 写入 bag。

Local、TCP、UDP、HTTP 还会检查测试 Runtime 的 subscriber 接收日志；ROS2 还会
检查 bag 中的序列化格式为 `ros2`。

具体 transport 矩阵如下：

| Transport | 发布端配置 | 输出格式 | 通过时必须看到 |
|-----------|---------------------|--------------------|--------------------------|
| Local | Runtime publisher + Runtime subscriber；recorder Tap | `auto`（实际为 CDR） | bag 中有 Local topic 的消息；Runtime subscriber 收到消息 |
| DDS | Runtime publisher；recorder 原生序列化订阅 | `cdr` | recorder 原生订阅采集到 DDS topic 并写入 bag |
| Iceoryx | Runtime publisher；recorder 缓存类型订阅 | `cdr` | recorder 独立订阅采集到 Iceoryx topic 并写入 bag |
| MQTT | Runtime publisher；recorder 缓存类型订阅 | `cdr` | recorder 独立订阅采集到 MQTT topic 并写入 bag |
| Zenoh | Runtime publisher；recorder 原生序列化订阅 | `cdr` | recorder 原生订阅采集到 Zenoh topic 并写入 bag |
| ROS2 | Runtime publisher；recorder 原生序列化订阅 | `ros2` | recorder 原生订阅采集到 ROS2 topic 并写入 bag，序列化格式为 `ros2` |
| TCP | Runtime publisher + Runtime subscriber；recorder Tap | `cdr` | bag 中有 TCP topic 的消息；Runtime subscriber 收到消息 |
| UDP | Runtime publisher + Runtime subscriber；recorder Tap | `cdr` | bag 中有 UDP topic 的消息；Runtime subscriber 收到消息 |
| HTTP | Runtime publisher + Runtime subscriber；recorder Tap | `cdr` | bag 中有 HTTP topic 的消息；Runtime subscriber 收到消息 |


## CLI 功能如何验证

下表列出各项 CLI 功能的测试方法和结果判据。

| CLI 功能 | 测试方法 | 通过判据 |
|----------|----------|----------|
| 显式 topic、`--output` | 指定一个 topic 和每个用例唯一的输出目录 | 目标目录能被 `bag info` 读取，只含指定 topic 且至少 5 条 |
| `--all` | 同时发布两个公开 topic，并另设 hidden 和 subscriber-only topic | 默认只录两个公开 topic，并且两个 topic 分别至少 5 条 |
| `--regex`、`--exclude` | 正则先选择 data/other，再排除 other | bag 只含 data，证明 include 和 exclude 都生效 |
| hidden/unpublished/leaf | 用 `--include-hidden-topics`、`--include-unpublished-topics` 加入特殊 topic，再用 `--ignore-leaf-topics` 排除 leaf | hidden 有消息；unpublished leaf 有 channel 但 0 条；ignore 后 leaf 不存在 |
| `--no-discovery` | recorder 先启动，Runtime 后启动 | bag 保持为空；对照的动态发现用例能录到 late topic |
| `--polling-interval` | late Runtime 在 recorder 启动后才出现，分别使用短轮询和 1500 ms 长轮询 | 短轮询能发现并录制；长轮询用例在观察窗口内保持为空 |
| `--storage` | 分别写 MCAP 和 SQLite3 | `bag info` 的 storage id 与参数一致，文件可完整读取 |
| storage preset | SQLite3 使用 `resilient` | 数据库实际为 WAL，`synchronous` 为 FULL 对应的值 2 |
| storage config | 使用测试目录自己的 SQLite YAML | 数据库实际切换为 YAML 指定的 WAL |
| `--serialization-format` | 覆盖 `auto`、`cdr`、`json`、`ros2`，另用 CDR publisher 指定输出 JSON | CDR 输入被真正转成 JSON，bag 显示 JSON serialization 和 JSON schema |
| `--max-cache-size` | snapshot cache 限制为 128 bytes | 快照仍有 1 至 4 条消息，同时 dropped 大于 0，证明 cache 确实被限制 |
| `--max-bag-size` | 设置 1200 bytes | 生成多个文件，metadata 的 split reason 为 size |
| `--max-bag-duration` | 设置 0.2 秒 | 生成多个文件，metadata 的 split reason 为 duration |
| compression mode/format | 分别运行 message/zstd 和 file/lz4 | `bag info` 显示对应压缩方式；file 模式实际生成 `.mcap.lz4` 文件 |
| compression queue/threads | queue 设为 2；threads 分别设为 0 和 2 | metadata 中 queue 为 2；0 被解析为非 0 CPU 线程数；显式值为 2 |
| `--start-paused` | 运行不恢复和发送 SIGUSR2 恢复两组 | 不恢复时 0 条；恢复后目标 topic 至少 5 条并出现 resumed 日志 |
| `--snapshot-mode` | 运行不发送信号和发送 SIGUSR1 两组 | 不发信号时 0 条；发送后至少 5 条并出现 snapshot completed 日志 |
| `--use-sim-time` | 发布真实 `rosgraph_msgs/msg/Clock` 和数据 topic | `/clock` 和数据 topic 分别至少 5 条，bag 起始时间等于模拟时钟值 |
| `--log-level` | DDS 直连 recorder 分别使用 info 和 error | info 有 recorder 引擎 Info 日志；error 压掉同一日志，同时仍正常录制 |
| `--help` | 读取 `bag record --help` | record 的全部 CLI option 都存在 |

正常录制用例还会统一检查：

```text
Closed cleanly:    yes
Dropped messages:  0
Duplicate copies:  0
Complete playback: yes
```

`--start-paused` 未恢复、snapshot 未触发、`--no-discovery` 等用例预期为 0 条消息，
并使用空 bag 健康检查。`--max-cache-size` 用例预期出现 dropped，采用对应的缓存
限制判据。


## 如何判断整套测试是否通过

以脚本退出码为准：

- 退出码 0，并打印 `PASS: record smoke transport and CLI matrix`：全部通过；
- 退出码非 0，并打印 `FAIL: record smoke cases: ...`：列出的任意一项失败；
- 某个 transport 失败后，脚本会继续执行剩余 transport 和 CLI case，并最终返回
  非 0。


## 文件说明

| 文件 | 用途 |
|------|------|
| [validate_record_smoke.sh](./validate_record_smoke.sh) | 启动服务、Runtime 和 recorder，执行全部用例并验证 bag |
| [record_smoke_module.h](./managed/record_smoke_module.h) | 定义测试 Module 的 publisher、subscriber、Clock 和工作线程 |
| [record_smoke_module.cc](./managed/record_smoke_module.cc) | 读取 YAML，注册消息类型并持续发布测试消息 |
| [record_smoke_transport.h](./generated/record_smoke_transport.h) | 声明 CDR、ROS2、JSON 和 Clock 的消息适配接口 |
| [record_smoke_cdr_transport.cc](./generated/record_smoke_cdr_transport.cc) | 发送和接收 CDR `Header` 消息 |
| [record_smoke_ros2_transport.cc](./generated/record_smoke_ros2_transport.cc) | 发送和接收 ROS2 消息及真实 `Clock` 消息 |
| [record_smoke_json_transport.cc](./generated/record_smoke_json_transport.cc) | 发送和接收 JSON 消息 |
| [record_smoke_pkg_main.cc](./package/record_smoke_pkg_main.cc) | 注册 CDR、ROS2 和 JSON 三个测试 Module |
| [type_support_pkg_main.cc](./type_support_pkg_main.cc) | 导出 Header 的 CDR/JSON 转码能力，以及 ROS2、Clock、JSON TypeSupport/SchemaSupport |

所有测试配置位于 `config/linux/bin/cfg`，均由 `record_smoke_test` 独立维护。
