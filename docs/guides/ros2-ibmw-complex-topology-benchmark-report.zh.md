# ROS2 与 IBMW 复杂拓扑性能对标报告

> **2026-09-29 审计更正｜引用本报告时优先采用本注释。** 下文保留 2026-09-07 历史测试记录与结果表，不代表本轮重新测量或复算。归档源码支持“普通 DDS 兼容路径的接收调度更短”：双方均使用普通发布、CDR 编解码和 relay 深拷贝，没有应用 loan；ROS2 为 `rclcpp::spin` 单线程执行器，IBMW 为未配置 executor 的 SYNC listener 内联回调，不能把收益归给 loan、共享载荷池或 Asio／TBB。
>
> **“相同 QoS／相同 Fast DDS”须收窄。** 归档 `config/relay.yaml` 仅配置 derived writer depth 32；raw reader 缺少 driver subscriber QoS 配置，按驱动默认为 10，ROS2 则为 32。因此 §1、§4.2、§10.1、§12 的全量一致性表述不获源码支持。双方 Fast DDS／CDR 兼容不证明实际加载构建、全部运行时 QoS 或传输相同；默认 UDPv4＋SHM 不足以断言当次仅 UDP 或使用 SHM loan。
>
> **测量与因果分开。** 主机 P99 618.9→444.5 us、总 CPU 3.67→2.80% 是历史报告值；relay／sink 降低而 source 上升，支持接收短路径假设，不能证明各机制贡献比例。同步业务占用 DDS 调用线程，重回调可能阻碍后续接收。§8 中注册和 match 等待属于启动阶段，不能直接解释 §5.4 的稳态 CPU。t0 实际在 Make 分配填充前；t1 在 relay 校验与 vector 深拷贝后、覆盖时间戳和发送前；t2 在 sink 回调入口、校验前，故 E2E 含准备与复制，hop 1 不是纯传输时间。
>
> **复现证据仍有缺口。** §11 四个 `/tmp/opencode` 原始结果／分析器／汇总路径本轮均不存在，无法复算逐轮结果。归档 OHOS 脚本对两 lane 都等待 source.ready／relay.ready，但 ROS2 endpoint 未实现 readiness 文件和 start gate，精确运行来源待恢复；下文 5/5 与 10/10 等仍是历史记录，不能视为此次重新确认。§8.3／§8.4 的旧百分比文字已按 §7.2 表格纠正，结果表未改。

**报告版本：** V1.0
**测试日期：** 2026-09-07
**测试场景：** 单机四进程、两跳转发、双订阅者扇出
**对标对象：** ROS2 Humble 原生路径与 IBMW DDS ROS2-compatible 路径
**源码基线：** `a795ac4a5551b818d2004e567a01bc177ca5aea5` 加本报告对应 topology benchmark 工作树变更

## 1. 执行摘要

本次测试在 x86_64 主机和 OHOS AArch64 实机上，使用相同消息类型、载荷、QoS、发送频率、进程边界和 Fast DDS 协议，构造了 `source -> relay -> sink A/sink B` 的四进程复杂拓扑。测试覆盖两跳端到端延迟、单跳分解、双订阅者扇出一致性、四进程稳态 CPU 以及 PSS 内存占用。

在成功完成 discovery 并通过完整性校验的运行中，IBMW 路径表现出以下优势：

| 平台 | E2E P50 | E2E P95 | E2E P99 | 四进程稳态 CPU | 四进程 PSS |
|---|---:|---:|---:|---:|---:|
| x86_64 主机 | 降低 31.1% | 降低 26.5% | 降低 28.2% | 降低 23.6% | 降低 4.5% |
| OHOS AArch64 | 降低 22.8% | 降低 22.3% | 降低 4.5% | 降低 41.4% | 降低 43.7% |

综合判断：

1. **性能结论明确。** 在本次受控拓扑中，IBMW 在两平台的典型延迟和稳态 CPU 上均优于 ROS2 原生路径。
2. **资源优势在 OHOS 更突出。** IBMW 四进程 PSS 为 30,926 KiB，ROS2 为 54,902 KiB，降低 43.7%。
3. **主机结果稳定。** ROS2 与 IBMW 均为 5/5 次有效，所有有效运行均无丢失、重复、乱序和非法载荷。
4. **OHOS discovery 稳定性已完成修复验证。** ROS2 和 IBMW 均为最终矩阵 5/5 次有效；此前复现的 IBMW discovery 零消息问题通过 source/relay 显式 readiness barrier 和完整 teardown 等待修复。另行执行的 10 次连续 IBMW 稳定性回归也全部通过，每个 sink 均收到 3000/3000 条消息。

面向领导汇报，可使用以下结论：

> 在统一 Fast DDS 协议和相同业务拓扑下，IBMW 的轻量兼容路径在主机和 OHOS 实机上均表现出更低的典型端到端延迟、稳态 CPU 和进程内存占用；OHOS discovery 已通过 readiness barrier 修复及连续稳定性回归验证。后续仍应在更长时间、多设备和更高负载矩阵下继续验证。

## 2. 测试目标与边界

### 2.1 测试目标

- 验证 IBMW 在多进程、两跳转发和一对多扇出场景下的端到端性能。
- 在相同 DDS 后端和消息语义下，对比 ROS2 完整运行时路径与 IBMW DDS 兼容路径的框架开销。
- 同时验证延迟、CPU、内存和消息正确性，避免只用单一平均延迟判断优劣。
- 在主机与 OHOS 实机上验证趋势是否一致。

### 2.2 结论边界

本测试比较的是两条**完整中间件框架路径**：

```text
ROS2 lane : Application -> rclcpp -> rcl/rmw_fastrtps_cpp -> Fast DDS
IBMW lane : Application -> IBMW Engine/Transport/ROS2 type adapter -> Fast DDS
```

因此，结果反映 API 层、执行器、回调调度、序列化适配、运行时服务和 DDS 接入方式的总开销，不是 Fast DDS 内核之间的对比，也不能直接外推到 Cyclone DDS、Connext、Zenoh、共享内存或跨机网络。

## 3. 测试环境

### 3.1 硬件与操作系统

| 项目 | x86_64 主机 | OHOS 实机 |
|---|---|---|
| CPU | Intel Core i7-14700 | HUAWEI Kirin X90 |
| CPU 拓扑 | 20 核、28 线程 | 20 个 AArch64 逻辑 CPU，异构核心 |
| 内存 | 125 GiB | 32,680,216 KiB，约 31.2 GiB |
| 架构 | x86_64 | AArch64 |
| OS/内核 | Ubuntu 22.04，Linux 5.15.0-136-generic | HarmonyOS/OHOS，HongMeng Kernel 1.12.0 |
| NUMA | 单节点 | 单机 SoC |
| CPU tick | 100 Hz | 100 Hz |

OHOS 设备 wall-clock 曾显示 2001 年日期，因此本测试不使用 wall-clock 计算延迟，只使用同机单调时钟。设备报告的产品标识为 `JVS`，硬件字段为 `HUAWEI KirinX90`。

### 3.2 软件栈

| 组件 | 主机 | OHOS |
|---|---|---|
| ROS2 | Humble | Humble target build |
| rclcpp | 16.0.17 | 16.0.14 |
| rmw_fastrtps_cpp | 6.2.9 | 6.2.8 |
| Fast DDS | 2.6.11 | 2.6 系列；IBMW target SDK 为 2.6.12 |
| Fast CDR | 1.0.29 | 1.0.29 |
| IBMW | 当前源码工作树 | 同源码交叉编译 AArch64 ELF |
| 编译器 | GCC 11.4.0 | OHOS Clang 15.0.4 |
| CMake | 3.28.0 | 3.28.0 驱动交叉编译 |
| 设备工具 | 不适用 | HDC 3.2.0c |

主机 ROS2 lane 显式设置 `RMW_IMPLEMENTATION=rmw_fastrtps_cpp` 和 `ROS_DOMAIN_ID=45`。OHOS ROS2 lane 使用相同 RMW 与 domain。IBMW DDS 配置启用 `ros2_compatible: true`，使用 ROS2 `rt` topic 前缀和 domain 45。

## 4. 拓扑模型

### 4.1 进程与消息流

```text
                              single host / single monotonic clock

 +------------------+       /mw_perf/topology/raw       +------------------+
 | Process 1        |  --------------------------------> | Process 2        |
 | SOURCE           |                                    | RELAY            |
 |                  |                                    |                  |
 | seq              |                                    | validate raw     |
 | origin_send=t0   |                                    | relay_receive=t1 |
 +------------------+                                    +---------+--------+
                                                                  |
                                      /mw_perf/topology/derived    |
                                             fanout 1:N            |
                                                  +---------------+---------------+
                                                  |                               |
                                                  v                               v
                                        +------------------+            +------------------+
                                        | Process 3        |            | Process 4        |
                                        | SINK A           |            | SINK B           |
                                        | receive=t2A      |            | receive=t2B      |
                                        | validate + JSONL |            | validate + JSONL |
                                        +------------------+            +------------------+

 Metrics:
   hop 1 latency    = t1  - t0
   hop 2 latency    = t2X - t1
   end-to-end       = t2X - t0
   fanout skew      = abs(t2A - t2B), matched by seq
```

四个角色均为独立 OS 进程。ROS2 lane 的四个进程全部运行原生 ROS2 endpoint；IBMW lane 的四个进程全部运行 IBMW Engine + DDS endpoint，避免混合路径干扰归因。

### 4.2 消息合同

| 项目 | 固定值 |
|---|---|
| Topic 1 | `/mw_perf/topology/raw` |
| Topic 2 | `/mw_perf/topology/derived` |
| 消息类型 | `std_msgs/msg/UInt8MultiArray` |
| Payload | 4096 bytes |
| QoS | reliable、volatile、keep-last 32 |
| 发送频率 | 100 Hz |
| Warmup | 50 条 |
| Measured | 3000 条 source messages/run |
| DDS domain | 45 |
| 扇出 | relay publisher 对两个 sink subscribers |

Payload 内嵌可验证元数据：

```text
bytes  0.. 7 : sequence
bytes  8..15 : origin monotonic send timestamp
bytes 16..23 : relay callback monotonic timestamp
bytes 24..31 : magic 0x544F504F4C4F4759
bytes 32..4095: fixed fill 0xA5
```

每条有效消息必须满足：

```text
origin_send_ns <= relay_receive_ns <= sink_receive_ns
```

## 5. 测试方法

### 5.1 启动与 discovery 门禁

1. runner 启动 source 后立即暂停 source，读取真实 PID 和 `/proc/<pid>/stat` start ticks。
2. runner 启动 relay、sink A 和 sink B，并将 source identity 传给两个 sink artifact。
3. relay 的 derived publisher 必须匹配两个 sink 后才能进入转发阶段。
4. source 的 raw publisher 必须匹配 relay 后才能开始发送。
5. runner 恢复 source，开始 50 条 warmup 和 3000 条 measured 消息。

该流程避免 publisher 在订阅端完成 discovery 前发送，从而降低 volatile QoS 下的启动丢包风险，同时将 artifact 绑定到真实 producer 进程。

### 5.2 重复与顺序控制

- 每个平台目标为 5 个有效 ROS2 runs 和 5 个有效 IBMW runs。
- 奇数 block 按 ROS2 -> IBMW 顺序执行，偶数 block 按 IBMW -> ROS2 顺序执行。
- lane 严格串行，运行间设置 cooldown，避免两个 lane 同时争用 CPU 和 DDS domain。
- 主机 ROS2 与 IBMW 均为 5/5 有效运行。
- OHOS ROS2 与 IBMW 最终矩阵均为 5/5 有效运行；修复后另行执行 10 次连续 IBMW 稳定性回归，10/10 通过。

最终 OHOS 汇总使用修复后的完整 5 组交替 block；稳定性回归用于验证启动/发现可靠性，不与正式性能样本合并。结果仍属于受控单设备描述性比较，不能替代更大规模生产验证。

### 5.3 延迟采集

- 时钟：进程内 `CLOCK_MONOTONIC` 对应的纳秒时间。
- `t0`：source 调用 publish/send 前。
- `t1`：relay callback 收到 raw 后、转发 derived 前。
- `t2`：sink callback 入口。
- 每个 sink 写入独立 `mw_perf.topology.v1` JSONL。
- 独立分析器不信任 endpoint CSV 汇总，而是从原始时间戳重新计算 hop 1、hop 2、E2E 和 fanout skew。

### 5.4 CPU 采集

CPU 使用 `/proc/<pid>/stat` 的 `utime + stime` 与 `/proc/uptime` 计算：

```text
process_cpu_pct = delta(utime + stime) / CLK_TCK / elapsed_seconds * 100
total_cpu_pct   = sum(source, relay, sink A, sink B)
```

- 主机窗口约为 source 恢复后的第 5 至 20 秒，实际约 15 秒。
- OHOS 在第 5 秒采集起点，执行中点内存和 `top` 采集后记录终点，实际窗口约 16.3 秒。
- 计算使用实际 uptime 差值，不假设固定 elapsed。
- 百分比以单个逻辑 CPU 为 100%，四进程数值可以相加。
- 该指标表示稳态通信 CPU，不包含完整 discovery、启动和退出成本。

### 5.5 内存采集

- PSS 来自 `/proc/<pid>/smaps_rollup`。
- RSS 来自 `/proc/<pid>/status`，仅作为交叉检查，不作为主要 footprint 结论。
- 四进程 PSS 求和用于估算该拓扑的实际进程内存占用，并尽量避免共享页被重复计算。
- 主机 PSS 在约第 5 秒采样；OHOS PSS 在约第 12 秒采样。
- 这是单点稳态 footprint，不代表启动峰值或长期内存增长。

### 5.6 工具与证据

| 工具 | 用途 |
|---|---|
| CMake/Ninja | 主机和 OHOS endpoint 构建 |
| GCC/Clang | x86_64 与 AArch64 编译 |
| HDC | OHOS 部署、远程执行和 artifact 拉取 |
| `/proc/<pid>/stat` | CPU ticks、PID start identity |
| `/proc/uptime` | CPU 采样实际时间窗口 |
| `/proc/<pid>/smaps_rollup` | PSS |
| `/proc/<pid>/status` | RSS 与进程状态 |
| `top -b` | OHOS 四进程资源快照和 PID 旁证 |
| Python 3 | runner、JSONL 严格校验、独立统计复算 |
| JSONL/JSON/CSV | 原始逐消息证据、状态和辅助摘要 |

## 6. 正确性门禁

每个有效 run 必须同时满足：

- source 和两个 sink 正常完成；relay 在 sink 完成后受控退出。
- sink A 和 sink B 各有 3000 条 measured records 和 1 条 completion record。
- sequence 集合严格等于 `[50, 3049]`，无缺号、重复和越界。
- 两个 sink 的 sequence 集合完全一致。
- payload 长度、magic 和 fill bytes 全部正确。
- `origin <= relay <= receive`，且 JSONL latency 与时间戳差值一致。
- run ID、lane、role、sink、topic、type 和 producer identity 一致。
- status 中 missing、duplicate、out_of_order、invalid_payload、invalid_timestamp 均为 0。

每个 lane 的 5 个有效 runs 共包含 15,000 条 source messages，在两个 sink 形成 30,000 条 records。两个 sink records 由同一 source message 派生，不应视为 30,000 个相互独立样本。

## 7. 测试结果

统计口径为：每个 run 合并 sink A 与 sink B 的 6,000 条 records，P50 使用中位数，P95/P99 使用 nearest-rank（`ceil(p*n)`）算法；先计算每个 run 的 P50/P95/P99/Max，再取 5 个有效 runs 对应统计量的中位数，记为“median-run percentile”。表中的 Max 是 5 个 run max 的中位数，不是全体运行的绝对最大值。

### 7.1 x86_64 主机结果

| 指标 | ROS2 | IBMW | IBMW 相对变化 |
|---|---:|---:|---:|
| E2E P50 | 355.8 us | 245.0 us | -31.1% |
| E2E P95 | 539.9 us | 396.6 us | -26.5% |
| E2E P99 | 618.9 us | 444.5 us | -28.2% |
| median-run Max | 746.7 us | 509.8 us | -31.7% |
| 全部成功运行绝对 Max | 1,103 us | 557 us | -49.5% |
| 四进程稳态 CPU | 3.67% | 2.80% | -23.6% |
| 四进程 PSS | 39,406 KiB | 37,647 KiB | -4.5% |
| 有效运行 | 5/5 | 5/5 | 相同 |

#### 两跳与扇出分解

| 指标 | ROS2 | IBMW | IBMW 相对变化 |
|---|---:|---:|---:|
| source -> relay P50 | 204.3 us | 133.7 us | -34.6% |
| source -> relay P99 | 375.6 us | 279.6 us | -25.6% |
| relay -> sink P50 | 162.7 us | 102.2 us | -37.2% |
| relay -> sink P99 | 310.5 us | 216.5 us | -30.3% |
| fanout skew P50 | 58.4 us | 45.3 us | -22.4% |
| fanout skew P99 | 236.6 us | 163.1 us | -31.0% |

#### 角色级稳态资源

| 角色 | ROS2 CPU | IBMW CPU | ROS2 PSS | IBMW PSS |
|---|---:|---:|---:|---:|
| source | 0.73% | 0.87% | 9,858 KiB | 9,653 KiB |
| relay | 1.33% | 1.00% | 9,928 KiB | 9,842 KiB |
| sink A | 0.80% | 0.47% | 9,678 KiB | 9,186 KiB |
| sink B | 0.80% | 0.47% | 9,813 KiB | 9,078 KiB |

主机结果显示，IBMW 的 CPU 优势主要来自 relay 和两个 sink；IBMW source CPU 略高于 ROS2 source。角色级中位数分别计算，因此可能与“每轮总和再取中位数”的总 CPU/PSS 存在轻微加和差异。

### 7.2 OHOS AArch64 实机结果

| 指标 | ROS2 | IBMW 成功运行 | IBMW 相对变化 |
|---|---:|---:|---:|
| E2E P50 | 1,953.0 us | 1,507.2 us | -22.8% |
| E2E P95 | 5,806.7 us | 4,510.5 us | -22.3% |
| E2E P99 | 8,740.9 us | 8,347.0 us | -4.5% |
| median-run Max | 13,828.3 us | 12,818.3 us | -7.3% |
| 四进程稳态 CPU | 38.97% | 22.83% | -41.4% |
| 四进程 PSS | 54,902 KiB | 30,926 KiB | -43.7% |
| 有效运行/总尝试 | 5/5 | 5/5 | 相同 |

#### 两跳与扇出分解

| 指标 | ROS2 | IBMW 成功运行 | IBMW 相对变化 |
|---|---:|---:|---:|
| source -> relay P50 | 870.5 us | 665.5 us | -23.5% |
| source -> relay P99 | 5,992.3 us | 5,859.1 us | -2.2% |
| relay -> sink P50 | 925.5 us | 766.8 us | -17.1% |
| relay -> sink P99 | 6,858.0 us | 6,174.7 us | -10.0% |
| fanout skew P50 | 98.0 us | 70.9 us | -27.7% |
| fanout skew P99 | 2,414.5 us | 1,843.4 us | -23.7% |

#### 角色级稳态资源

| 角色 | ROS2 CPU | IBMW CPU | ROS2 PSS | IBMW PSS |
|---|---:|---:|---:|---:|
| source | 4.16% | 6.09% | 13,680 KiB | 8,009 KiB |
| relay | 13.95% | 10.07% | 13,864 KiB | 8,265 KiB |
| sink A | 10.47% | 3.24% | 13,684 KiB | 7,335 KiB |
| sink B | 10.53% | 3.48% | 13,681 KiB | 7,364 KiB |

OHOS 上 IBMW 的 CPU 优势高度集中在两个 sink：ROS2 sink 约为 10.5%，IBMW sink 约为 3.2%-3.5%，单 sink 降幅约 66.9%-69.2%；relay CPU 也明显下降。与主机一致，IBMW source CPU 高于 ROS2 source，说明整体优势不是所有角色统一变快，而是接收、回调和转发链路节省的开销超过发送侧增量。

### 7.3 运行间离散度

| 平台/lane | run P50 范围 | 四进程 CPU 范围 |
|---|---:|---:|
| 主机 ROS2 | 308.6-379.8 us | 3.27%-4.13% |
| 主机 IBMW | 215.9-262.4 us | 2.53%-3.00% |
| OHOS ROS2 | 1,887.2-2,118.6 us | 38.11%-40.93% |
| OHOS IBMW | 1,457.8-1,614.1 us | 22.15%-23.34% |

有效运行中的方向一致：IBMW 的 P50 和总 CPU 在每个平台均低于 ROS2 对应范围。但样本量仅为 5，结果应视为受控环境下的描述性证据，不宣称统计显著性。

## 8. IBMW 更优的可能原因

以下分析分为数据直接支持和架构推测。架构推测用于指导后续 profiling，不等同于已经通过火焰图或硬件计数器证明的因果结论。

### 8.1 更短的运行时调用链

ROS2 lane 的数据路径需要经过 `rclcpp`、`rcl`、`rmw` 和 `rmw_fastrtps_cpp`，再进入 Fast DDS。IBMW lane 保留 ROS2-compatible wire/type 语义，但通过 IBMW transport 和 ROS2 type adapter 进入 Fast DDS。两条路径的实现差异与更短调用路径假设一致，但具体因果仍需 profiling 验证。

较短路径可能减少：

- API 层对象和共享指针包装；
- RMW 动态分派及状态转换；
- executor wait-set 与 callback 调度层级；
- 消息元数据与类型支持的重复适配；
- 接收侧临时对象、引用计数和内存分配。

这一推测与角色级结果一致：主机和 OHOS 上，IBMW 的 relay 与 sink CPU 均低于 ROS2；而 source CPU 没有同方向优势，说明节省更可能发生在接收回调和转发路径。

### 8.2 接收侧执行模型更轻

OHOS 上每个 ROS2 sink CPU 约为 10.5%，IBMW sink 约为 3.3%-3.5%，差异显著。可能原因包括：

- IBMW subscriber callback 到业务处理之间的调度层级更少；
- ROS2 executor、wait-set 和 guard-condition 在目标系统上的固定成本更高；
- IBMW 对该单一类型和单 transport 场景采用更直接的回调路径；
- OHOS libc++、线程唤醒和 ROS2 runtime 组合放大了抽象层开销。

要将该推测升级为确定结论，需要对四个角色分别采集 sampling profiler、线程切换、futex/wakeup、allocator 和 Fast DDS listener 调用栈。

### 8.3 更小的目标运行时依赖面

OHOS 四进程 PSS 从 ROS2 的 54,902 KiB 降至 IBMW 的 30,926 KiB。IBMW DDS endpoint 的构建目标未显式链接完整 `rclcpp/rcl/rmw` client 路径，预计因此减少部分运行时依赖，可能减少：

- ROS2 client library 与 RMW 共享库映射；
- logging、statistics、tracing 等运行时组件；
- executor、node graph 和参数服务相关状态；
- 每进程初始化的 runtime metadata。

主机 PSS 只降低 4.5%，而 OHOS 降低 43.7%（2026-09-29 按 §7.2 更正原文 43.5%），说明该优势与平台构建、动态库共享、符号/调试配置和 target runtime 组合强相关，不能假设所有 Linux 发行版都能得到相同幅度。

### 8.4 为什么 OHOS P99 优势弱于 P50/P95

OHOS E2E P50 降低 22.8%，P95 降低 22.3%，但 P99 仅降低 4.5%（2026-09-29 按 §7.2 更正原文 19.3%／28.9%／7.8%）。可能原因包括：

- Kirin X90 为异构 20 核，进程未绑核，调度迁移和不同核心性能影响长尾；
- 四个进程和 DDS 内部线程存在唤醒链，尾部事件更容易叠加调度延迟；
- 设备上存在系统服务、频率调节和中断干扰；
- reliable DDS 的确认、history 和资源回收可能在少量消息上形成批处理开销；
- 当前 payload 为 4 KiB，框架固定成本和 OS 调度成本均占较高比例。

后续应增加 CPU affinity、performance governor、实时优先级隔离以及 P99.9/P99.99 样本量，再判断长尾是否来自框架或平台调度。

### 8.5 为什么 IBMW source CPU 略高

主机与 OHOS 上，IBMW source CPU 均略高于 ROS2 source。可能原因包括 IBMW Engine 生命周期线程、发送线程、类型适配注册、publication match 查询或当前发送循环实现的固定开销。该结果说明优化重点应继续放在：

- source 侧线程与 timer 唤醒；
- send path 的类型查找和序列化缓存；
- publication match 轮询；
- Engine 在单模块 publisher 场景下的基础线程成本。

IBMW 当前总 CPU 仍然更低，但不能据此声称每个组件都比 ROS2 更轻。

## 9. OHOS Discovery 稳定性分析

### 9.1 现象

修复前 OHOS IBMW 曾出现 discovery 零消息失败：失败运行的两个 sink 均收到 0 条 measured 消息，status 正确记录 `valid=false`，没有部分接收、重复、乱序或 payload 损坏。修复后最终矩阵和 10 次连续稳定性回归均未再出现该问题。

### 9.2 初步推测

该模式表现为 discovery/match 未完成或未稳定；目前不能仅凭现有日志断言具体根因。可能方向包括：

- Fast DDS participant 在连续创建和销毁后的 discovery 状态或共享资源清理；
- IBMW `DdsTransportDriver` publication-match 可见性或 topic-name 转换时序；
- relay publisher 已匹配两个 sink，但 source/relay 的跨 participant 匹配状态未稳定；
- OHOS 上 participant teardown、端口复用或线程退出存在延迟；
- runner 原有的短 cooldown 可能不足以覆盖目标设备的 participant teardown 延迟。

以上均为待验证假设。需要增加 participant GUID、matched count 变化、endpoint discovery callback、topic/type/QoS 摘要和 teardown 完成时间日志，才能确定根因。

### 9.3 产品判断

- **性能维度：** 最终 5 组有效运行中，IBMW 的典型延迟、稳态 CPU 和 PSS 均低于 ROS2。
- **稳定性维度：** 修复后最终矩阵 5/5 有效，另有 10/10 次连续 IBMW 稳定性回归通过。
- **综合维度：** 本受控拓扑下，OHOS IBMW 同时具备性能优势和已验证的启动/发现稳定性；仍需更大规模生命周期压力测试后再形成产品 SLA。

建议继续完成至少 1000 次启动/停止压力测试，并在多设备、长时间和异常退出场景下验证后，再将性能结果用于正式产品 SLA 或客户承诺。

## 10. 推广建议

### 10.1 推荐表述

> 在相同 Fast DDS、4 KiB reliable 消息和四进程两跳双扇出拓扑下，IBMW 在 x86_64 主机上将典型 E2E 延迟降低约 31%、稳态 CPU 降低约 24%；在 OHOS 实机上将典型 E2E 延迟降低约 23%、稳态 CPU 降低约 41%、四进程 PSS 降低约 44%。这表明 IBMW 的轻量兼容路径在本次单机多进程机器人通信拓扑中具有明确的性能和资源潜力；OHOS discovery 已通过 readiness barrier 修复及连续回归验证，但更大规模压力验证仍需独立完成。

### 10.2 不推荐表述

- “IBMW 在所有 ROS2 场景都快 30%-40%。”
- “IBMW 的 DDS 比 Fast DDS 更快。”
- “OHOS 上 IBMW 已全面优于 ROS2。”
- “P99 已达到实时确定性要求。”
- “内存降低 44% 可外推到所有平台和业务。”

### 10.3 下一阶段建议

1. 建立至少 1000 次 OHOS 生命周期稳定性门禁，覆盖正常退出、异常退出和重复启动。
2. 在固定 CPU affinity 和 performance governor 下复测，扩大到每 run 至少 10,000 measured messages。
3. 增加 P99.9、置信区间和严格配对 block 统计。
4. 对 source、relay、sink 分别采集火焰图、上下文切换、futex 和 allocator 数据，验证优化原因。
5. 扩展到 16 KiB、64 KiB、256 KiB 和 1 MiB payload，区分固定框架开销与序列化/拷贝开销。
6. 增加 `1->N` 不同 fanout 数量、双向 RPC、跨机网络和过载容量测试。
7. 将 topology runner 接入统一 `mw-perf-collect` comparison plan，自动记录 RMW、动态库 build-id、CPU governor、affinity 和设备温度。

## 11. 证据与复现入口

| 证据 | 路径 |
|---|---|
| Topology 源码与 runner | `src/samples/cpp/transport_benchmark/topology_interop/` |
| 主机正式原始结果 | `/tmp/opencode/topology-host-formal-final2/` |
| OHOS 拉回原始结果 | `/tmp/opencode/topology-ohos-final-results/` |
| 独立复算分析器 | `/tmp/opencode/analyze_topology.py` |
| 完整派生汇总 | `/tmp/opencode/topology-comparison-summary.json` |

已完成验证：

- 主机 ROS2 与 IBMW endpoint 构建成功。
- OHOS ROS2 与 IBMW AArch64 endpoint 构建成功。
- 两个平台所有纳入统计的 JSONL 均通过 sequence、时间戳、payload、completion、同文件 run identity 和 fanout sequence-set 一致性复算；endpoint status 同时报告 valid。
- OHOS `top` 快照包含预期四个 PID。
- topology runner Python syntax、OHOS shell syntax 和 `git diff --check` 通过。
- 项目自有测试为 83 passed，另有 3 个 subtests passed。
- 全仓裸 `pytest -q` 会误收集工作树 `build-asan-retained/_deps` 内第三方 gmock 测试，并因缺少其测试二进制在 collection 阶段退出；这不是 topology 功能或数据验证失败。

## 12. 最终结论

本轮复杂拓扑测试证明：在统一 Fast DDS 协议、相同消息合同和相同四进程拓扑下，IBMW 的轻量兼容路径能够在 x86_64 和 OHOS 上降低典型通信延迟与稳态 CPU，并在 OHOS 上显著降低运行内存。数据支持 IBMW 在本次单机多进程机器人通信拓扑中具备性能和资源推广价值；跨机、多节点网络和更广泛具身智能负载仍需独立验证。

同时，OHOS discovery 失败曾暴露出性能之外的工程短板，已通过显式 source/relay readiness barrier、启动 gate 和完整 teardown 等待完成修复验证。当前最准确的阶段判断是：**IBMW 的复杂拓扑性能和资源优势已经得到双平台实测支持；主机与 OHOS 在本次最终矩阵中均稳定通过，但仍需扩大生命周期压力测试后再承诺产品 SLA。**
