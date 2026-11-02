# OHOS A/B 端到端延迟实机基线

本文记录 2026-09-04 在 OHOS AArch64 实机完成的探索性 A/B 基线：

- A：原生 ROS2 Humble，`rclcpp` + `rmw_fastrtps_cpp`；
- B：IBMW DDS extension 的 ROS2-compatible 路径；
- 结论边界：`formal_p0a=false`、`metric_availability=unsupported`、
  `valid_for_metric=false`、`valid_for_comparison=false`。

这些数据用于确认实机链路可运行并建立后续 formal run 的参考，不能发布为性能胜负结论。

## 测试合同

| 项目 | 固定值 |
|---|---|
| topic | `/mw_perf/payload_4kb` |
| type | `std_msgs/msg/UInt8MultiArray` |
| payload | 4096 bytes |
| QoS | reliable、volatile、keep-last 8192 |
| 拓扑 | 单机双进程，1 publisher + 1 subscriber |
| 负载 | 100 Hz、50 warmup、3000 measured |
| 时间边界 | producer `CLOCK_MONOTONIC` send 到 subscriber `CLOCK_MONOTONIC` receive |
| 正确性 | missing、duplicate、out_of_order、invalid_payload、invalid_timestamp 均为 0 |
| 重复策略 | 5 blocks；奇数块 A→B，偶数块 B→A；lane 严格串行 |

设备报告为 `HarmonyOS localhost HongMeng Kernel 1.12.0`、AArch64。设备 wall-clock
显示为 2001-02-08，不能用于运行间时间比较；本测试只使用同机 monotonic 差值。

## 构建

以下命令假设当前目录为 IBMW 仓库根目录：

```bash
export IBMW_ROOT="$PWD"
export OHOS_ROS2_PREFIX=/path/to/full-ohos-ros2/install
export OHOS_MINIMAL_SDK_PREFIX=/path/to/ros2-minimal-humble-ohos-arm64-fastrtps-product-sdk
export OHOS_SDK_NATIVE_ROOT=/path/to/ohos-sdk/native
export A_BUILD=/tmp/ibmw-ohos-abc-a
export B_BUILD=/tmp/ibmw-ohos-abc-b

AMENT_PREFIX_PATH="$OHOS_ROS2_PREFIX" \
PYTHONPATH="$OHOS_ROS2_PREFIX/lib/python3.12/site-packages" \
cmake -S src/samples/cpp/transport_benchmark/abc_interop/ohos_ros2_standalone \
  -B "$A_BUILD" -G Ninja \
  -DIBMW_OHOS_ROS2_PREFIX="$OHOS_ROS2_PREFIX" \
  -DOHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT" \
  -DOPENSSL_ROOT_DIR="$OHOS_SDK_NATIVE_ROOT/sysroot/usr" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$A_BUILD" --target ibmw_abc_ros2_endpoint -j2

cmake -S . -B "$B_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$IBMW_ROOT/cmake/toolchains/ohos-arm64-v8a.cmake" \
  -DOHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT" \
  -DOPENSSL_ROOT_DIR="$OHOS_SDK_NATIVE_ROOT/sysroot/usr" \
  -DCMAKE_BUILD_TYPE=Release \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_TESTS=OFF \
  -DIBMW_BUILD_WITH_ROS2=OFF \
  -DIBMW_BUILD_ROS2_TYPES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_DDS_PROVIDER=ROS2_MINIMAL_SDK \
  -DIBMW_ROS2_MINIMAL_SDK_PREFIX="$OHOS_MINIMAL_SDK_PREFIX" \
  -DIBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD=OFF
cmake --build "$B_BUILD" --target ibmw_abc_dds_endpoint -j2
```

A 使用独立 CMake project，不放开仓库级 `IBMW_BUILD_WITH_ROS2` 或被禁止的 cross
`rclcpp` tier。B 的 OHOS endpoint 只在 ROS2 types 与 DDS extension 同时启用时进入构建。

## 部署与运行

A、B 必须使用独立 library 目录，避免 full ROS2 prefix 与 minimal SDK 中不同 Fast DDS /
foonathan memory 版本互相覆盖。部署包是从各 endpoint 的递归 `DT_NEEDED` closure 组装的
最小 bundle，并额外加入动态加载的 RMW/typesupport 库；所选子目录不是完整 prefix 副本。

```bash
export BUNDLE=/tmp/ibmw-ohos-abc
mkdir -p "$BUNDLE"/{bin,lib-a,lib-b,cfg,share}
cp "$A_BUILD/ibmw_abc_ros2_endpoint" "$BUNDLE/bin/"
cp "$B_BUILD/ibmw_abc_dds_endpoint" "$BUNDLE/bin/"
cp "$B_BUILD/libibmw_dds_transport.so" "$BUNDLE/lib-b/"
cp -a "$OHOS_ROS2_PREFIX/lib/." "$BUNDLE/lib-a/"
cp -a "$OHOS_MINIMAL_SDK_PREFIX/lib/." "$BUNDLE/lib-b/"
cp -a "$OHOS_ROS2_PREFIX/share/ament_index" "$BUNDLE/share/"
cp "$B_BUILD/cfg/abc_interop/"{publisher,subscriber}.yaml "$BUNDLE/cfg/"
cp src/samples/cpp/transport_benchmark/abc_interop/{run_ohos_lane.sh,validate_ohos_artifact.sh} "$BUNDLE/"

hdc file send "$BUNDLE" /data/local/tmp/
hdc shell 'chmod 755 /data/local/tmp/ibmw-ohos-abc/bin/* \
  /data/local/tmp/ibmw-ohos-abc/run_ohos_lane.sh'

IBMW_ABC_ROOT=/data/local/tmp/ibmw-ohos-abc \
MATRIX_ID=ohos-ab-5block-YYYYMMDD \
src/samples/cpp/transport_benchmark/abc_interop/run_ohos_matrix.sh

mkdir -p /tmp/ibmw-ohos-ab-results
hdc file recv /data/local/tmp/ibmw-ohos-abc/runs \
  /tmp/ibmw-ohos-ab-results/
```

每个 lane 先启动 publisher，记录其 PID，并读取 `/proc/$pid/stat` 第 22 字段作为 producer
start-time ticks；runner 随后通过 `MW_PERF_PRODUCER_PID` 和
`MW_PERF_PRODUCER_START_TIME_TICKS` 将该身份交给 subscriber，再启动 subscriber。A 的
publisher 会等待 subscriber count，B 的 publisher 有 match gate，因此该顺序不会提前发送样本。

每轮必须检查：`sub-status.json` 全部正确性计数、3000 条 sample JSONL、连续
`seq=50..3049`、`latency_ns=receive_ns-send_ns`、以及最后一条 completion 的
`record_count=3000`、`complete=true`、`clock_domain=producer_monotonic`。

每个 lane 还会生成 CPU 证据：`cpu-uptime-start.txt`、`cpu-uptime-end.txt`、
`pub-proc-stat-start.txt`、`sub-proc-stat-start.txt`、对应的 `*-end.txt`、
`top-mid.txt`、`top-end.txt` 和 `cpu-summary.json`。summary 使用
`/proc/<pid>/stat` 的第 14、15 字段（utime、stime）、`/proc/uptime` 以及
`CLK_TCK=100` 计算：`cpu_pct = ((end_utime + end_stime) -
(start_utime + start_stime)) / (CLK_TCK * elapsed_seconds) * 100`。
`pair_cpu_pct` 是 publisher 与 subscriber CPU 百分比之和，允许两进程的
CPU 使用发生重叠；summary 缺失或数值无效时 lane 不会标记为 `valid=true`。

## 本次结果

本次构建的关键 SHA-256：

| artifact | SHA-256 |
|---|---|
| A endpoint | `19e0794ffbf33e8d259dc6dfbcab7bb2ab47cf9e490b767d379260b86e987b2c` |
| B endpoint | `78d396d981154bba48f5259f0314e037e2740442c53907bff116cc5962198a87` |
| `librmw_fastrtps_cpp.so` | `9b80a7373987dc7694a772e41a9cec1a1093a259972b659b26cc9ecf319f76e8` |
| `libibmw_dds_transport.so` | `837fa19466fa6760bed3f4b43c76df9ab874c163baf02ce414583ff4230a3886` |

所有 10 轮均为 3000/3000 unique，五项错误计数均为 0，严格 artifact completion
均为 `complete=true`。下表由原始 JSONL 重新计算 nearest-rank percentile；PSS/RSS
是 publisher 与 subscriber 的运行中点之和，单位 KiB。

| block | lane | p50 us | p90 us | p99 us | p99.9 us | pair PSS | pair RSS |
|---:|:---:|---:|---:|---:|---:|---:|---:|
| 1 | A | 957.656 | 2485.469 | 7267.969 | 8873.594 | 38094 | 67884 |
| 1 | B | 770.156 | 1836.875 | 7117.032 | 8999.063 | 23803 | 42652 |
| 2 | B | 718.125 | 1717.500 | 6690.782 | 8803.437 | 23861 | 42648 |
| 2 | A | 945.625 | 2530.468 | 6537.500 | 8187.344 | 38030 | 67748 |
| 3 | A | 947.187 | 2587.032 | 7532.500 | 9657.656 | 38081 | 67812 |
| 3 | B | 790.312 | 1882.813 | 6873.438 | 8590.156 | 23785 | 42656 |
| 4 | B | 692.500 | 1685.938 | 6220.625 | 9427.969 | 23874 | 42900 |
| 4 | A | 951.250 | 2550.938 | 7078.750 | 9428.594 | 38104 | 67884 |
| 5 | A | 961.562 | 2593.594 | 7172.969 | 9129.687 | 38015 | 67772 |
| 5 | B | 742.812 | 1884.844 | 6115.781 | 9189.219 | 23852 | 42728 |

| 指标 | A 的 5-block 中位数 | B 的 5-block 中位数 | paired median B/A |
|---|---:|---:|---:|
| p50 us | 951.250 | 742.812 | 0.773 |
| p90 us | 2550.938 | 1836.875 | 0.727 |
| p99 us | 7172.969 | 6690.782 | 0.913 |
| p99.9 us | 9129.687 | 8999.063 | 1.007 |
| pair PSS KiB | 38081 | 23852 | 0.627 |
| pair RSS KiB | 67812 | 42656 | 0.630 |

## 限制

- 样本只有一台设备、一次 5-block session，未计算置信区间或显著性；不能据此声明 winner。
- CPU 证据由同一 lane 的 uptime 和 proc stat 起止采样生成；`pair_cpu_pct`
  是两进程 CPU 百分比之和，不是去重后的机器总 CPU 使用率。
- RSS 会重复计算共享页，跨 lane 观察优先看同一采样点的 pair PSS。
- A 每个进程退出时报告 `Failed to delete participant`，但进程返回 0、完整 artifact
  已落盘且后续 block 未发现残留 endpoint；formal run 前仍应消除或资格化该告警。
- 本轮 artifact 未归档，不进入版本库。
- 本文不改变 OHOS physical formal P0a、通用 exact、其它 transport/type 的验收状态。
- 当前 standalone CMake lane 面向 Release 交付；未声明 OHOS sanitizer runtime 可用性，不能把 sanitizer 构建当作本 lane 的验证结果。

## CPU 采集补测

此前结果没有 CPU 数值，原因是初版 runner 只保存了中点 RSS/PSS，没有保存 CPU
时间边界。这是采集工具能力不足，不代表 CPU 无法测量。已在同一设备、同一 A/B
合同下重新执行 5 个交替 block，并增加：

- `top -b -n 1 -p <publisher>,<subscriber> -O %CPU,TIME+,PID,ARGS` 的中点和结束快照；
- 两个进程 `/proc/<pid>/stat` 的起止快照；
- `/proc/uptime` 起止值；
- `cpu-summary.json`。

subscriber 启动并完成初始 proc 设置后，publisher 和 subscriber 保持存活再等待固定
5 秒 startup settle；这 5 秒不计入 CPU measurement。随后才采集 CPU 起点。结束点
固定为起点后的 `floor((warmup_messages + message_count) / hz) - 5` 秒，最少 1 秒；
其中最后 5 秒 margin 用于 publisher-first discovery/startup。对于本合同的 100 Hz、50
warmup、3000 measured，CPU measurement 窗口为 20 秒，并在 publisher 自然结束前、两个进程
仍存活时采集。`cpu-summary.json` 使用这组相同的 uptime 和
proc stat 起止边界；后续等待 publisher 结束和停止 subscriber 不会覆盖结束边界。
若任一进程提前退出，CPU summary 和 runner 有效性检查失败，不拼接不一致的边界。
中点及结束点 `top` 快照保留为佐证，并纳入有效性门禁：两个文件都必须非空且都
包含 publisher 和 subscriber PID。

CPU 计算公式为：

```text
process_cpu_pct = ((delta_utime + delta_stime) / CLK_TCK) / elapsed_seconds * 100
pair_cpu_pct = publisher_cpu_pct + subscriber_cpu_pct
```

设备 `CLK_TCK=100`。`pair_cpu_pct` 是两个 benchmark 进程 CPU 百分比之和，允许
两个进程同时运行；它不是去重后的整机 CPU 使用率。重新采集的 10 次运行全部
通过 3000-record artifact 和 CPU summary 校验。

| 指标 | 原生 ROS2 A 中位数 | IBMW DDS B 中位数 | 显示中位数 B/A | 各 block B/A 比值的中位数 |
|---|---:|---:|---:|---:|
| publisher CPU | 4.109% | 7.048% |  | 1.715 |
| subscriber CPU | 11.712% | 3.286% |  | 0.281 |
| pair CPU | 15.884% | 10.338% | 0.651 | 0.668 |

因此，在本次单机双进程、100 Hz、4 KiB payload 测试中，B 的合计进程 CPU
各 block B/A 比值的中位数为 0.668，即约低 33.2%。另按表中显示的两个中位数
直接相除，`10.338 / 15.884 ~= 0.651`，对应约低 34.9%。CPU 分布发生了角色转移：B publisher 约高 71.5%，
subscriber 约低 71.9%。这只能作为当前设备和当前合同的探索性观察，不能推广
为所有负载或所有 OHOS 平台的结论。

CPU 补测结果未归档，不进入版本库，其中包含
每轮 `top` 快照、proc stat 快照和重新计算的 `exploratory-matrix-cpu.json`。
