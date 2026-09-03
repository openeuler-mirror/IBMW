<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# ROS 2 与 IBMW 性能采集及可观测性设计

> 状态：设计基线 v1。本文同时标明当前实现与后续规划，不能将规划项视为产品能力。通用逐消息时延的候选架构和待审核决策见[通用通信时延采集方案](generic-communication-latency-observation-proposal.zh.md)。

## 1. 目标与范围

本设计为 ROS 2/DDS 基线、IBMW DDS 兼容路径和 IBMW 其他 Transport 提供低侵入、可复现、可审计的性能证据。它用于回答：

- 原始 ROS 2/RMW/DDS 路径的性能与资源基线是什么；
- IBMW DDS 兼容路径的迁移成本是否可控；
- IBMW 新 Transport 在语义等价场景中的收益是什么；
- IBMW 内部的发送、Driver、loan、接收与回调路径是否能解释外部结果。

本专项固定为三条交付主线，A/B/C 是共同验证场景，不是第四条主线：

1. **ROS2 采集工具**：ROS 环境、Graph、readiness、进程资源和 producer-owned latency artifact；
2. **IBMW 采集工具**：IBMW 外部只读发现、进程资源、产物归档、来源校验和报告；
3. **IBMW 本体适配**：默认关闭的 Core Metrics、Reporter 与生命周期接入。

用户操作见[性能采集指南](../guides/performance-collection.zh.md)，稳定实施约束见[实施说明](../contributing/performance-observability-implementation.zh.md)。

## 2. 基本原则

### 2.1 低侵入边界

“低侵入”指不修改被测业务源码、消息定义和业务处理逻辑，不表示零扰动。CLI Graph 查询、`/proc` 采样、额外 artifact 写入和 Core Metrics counters 都可能产生开销，因此报告必须记录采集方式和能力降级。

当前 ROS2 Adapter 不创建 subscription，不提供 generic Observer。逐消息 latency 仅接受被测 benchmark 或业务进程按严格合同主动生成的 JSONL，collector 负责绑定运行身份、归档与验证，不推断不存在的逐消息因果关系。

### 2.2 公平对标与内部诊断分层

| 层级 | 数据来源 | 用途 |
|---|---|---|
| A：公共外部指标 | 相同采集框架对 ROS 2 与 IBMW 做外部采样 | 正式对标 |
| B：可比中间件指标 | 两边存在相同事件定义时 | 辅助解释 |
| C：IBMW 内部指标 | Core Metrics/Reporter | IBMW 归因，不单独决定胜负 |

IBMW 内部精确计数不能替代 ROS 2 外部近似值参与正式排名。未知或不可获得的值应标为 `unsupported`，不能写成 0。

### 2.3 两条测量轨道

- `normalized_benchmark`：统一负载、序号、正确性和统计方法，可用于满足合同后的正式比较；
- `passive_business_observation`：不修改真实业务，只报告可直接获得的环境、拓扑、资源和业务已有指标。

两条轨道不得混合排名。当前 ROS2 Graph/环境采集属于被动业务观测；producer-owned latency 可为受控 benchmark 提供直接证据，但是否可比较仍由场景合同、资源边界和 validation 共同决定。

## 3. A/B/C 对照与比较门禁

| 组别 | 数据路径 | 目的 |
|---|---|---|
| A | ROS 2 -> 当前 RMW/DDS | 原始基线 |
| B | IBMW -> DDS ROS 2 兼容路径 | 兼容迁移成本 |
| C | IBMW -> 新 Transport | 新路径收益或 Pareto 结果 |

比较对象是满足同一场景合同的端到端解决方案，而不是 Transport 名称。合同至少冻结：

- 主机与进程边界；
- topic/type、payload 和序列化要求；
- reliability、durability、history、depth 与顺序语义；
- offered load、warmup、样本窗口和 drain；
- completion、重复、乱序、丢失与 payload 校验规则；
- 纳入统计的业务、中间件、router/daemon 资源边界。

兼容等级定义如下：

| 等级 | 含义 |
|---|---|
| L1 | ROS API/source compatible |
| L2 | RMW 行为、ROS Graph 与 QoS compatible |
| L3 | DDS wire interoperable |
| L4 | 业务语义等价，底层协议可不同 |
| L5 | 改变可靠性、序列化或进程边界的非等价优化 Profile |

只有场景合同 hash、资源核算、正确性、负载和测量窗口均通过时才计算 B/A、C/A 或 C/B。C 路径不具备 DDS 协议级发现/可靠语义时必须单列，不得描述为 DDS 等价替代。

## 4. 统一工具架构

最终仓库布局为：

```text
tools/mw_perf_collect/
├── mw-perf-collect
├── cli.py
├── core.py
├── adapters/
│   ├── ros2.py
│   └── ibmw.py
└── schemas/
```

统一 CLI 为：

```text
mw-perf-collect <doctor|run|validate|report|compare>
```

共同内核负责 managed/attached 生命周期、PID identity、进程树、CPU/PSS/RSS、网络、artifact、validation 和 report。ROS2 与 IBMW Adapter 只补充各自的环境、拓扑、能力和专用 artifact。

当前 `doctor`、`run`、`validate`、`report` 已实现。统一 CLI 中的 `compare` 对版本化 comparison-plan 已具备 block/lane 合同和 fail-closed 校验；独立 A/B/C runner 可生成比较元数据，但两者都不是自动执行任意 A/B/C 方案并生成正式统计结论的完整平台。当前 P0a managed DDS binary trace 已接入 collector 合同；选定 capability `dds.ordinary.nonplain.single_driver.sync_callback` 的 formal P0a completion 轴已在 system DDS、Fast DDS 2.6.12 与 3.2.2 通过，状态为 `valid_for_p0a_completion=true`，但 metric 轴仍 unsupported/invalid，且不是通用 exact。

## 5. 生命周期

### 5.1 状态模型

```text
PREFLIGHT -> PREPARE -> START_COLLECTORS -> START_SUT/SKIPPED_START_SUT
          -> WARMUP -> SAMPLE -> STOP_SUT/SKIPPED_STOP_SUT
          -> VALIDATE -> REPORT -> PACKAGE
```

当前实现覆盖上述单机最小闭环。设计完整态还包括独立 load/readiness/drain 阶段、cgroup/pidfd 强隔离和多机 Controller/Agent；这些增强项不得写成当前能力。

### 5.2 managed 与 attached

- `managed`：工具启动新 session 中的 SUT，记录 leader 及后代 PID/start ticks，采样后先 TERM、超时再 KILL；只有受控优雅结束才能把 required final artifact 判为 supported。
- `attached`：附着显式 PID，持续校验 PID identity，不启动也不停止目标。attached 中的 Core Metrics 或 latency artifact 无受控 finalization，optional 时仅作诊断，required 时 fail-closed。

工具只处理其 managed 运行中跟踪到的进程，不使用无范围的 `pkill`。

## 6. ROS2 采集设计

### 6.1 当前已实现

- ROS 环境变量、ROS CLI 可达性和 RMW 环境信息；
- 只读 `ros2 node list` 与 `ros2 topic list` 快照；
- 有界 readiness 轮询、topic presence 和 PID start ticks 双重校验；
- 公共进程、PSS/RSS、线程和网络采样；
- producer-owned latency JSONL 的运行身份注入、严格校验、hash 与归档；
- ROS 不可用时显式降级，不合成指标。

latency 数据记录固定字段：

```text
schema, seq, send_ns, receive_ns, latency_ns,
topic, type, payload_bytes, valid
```

末行必须是唯一 completion record，绑定 collector 注入的 `run_id`、producer PID/start ticks、记录数及 topic/type。空行、截断、NaN、seq 重复/倒序、跨 topic/type、时间公式不一致或 completion 不匹配均拒绝。

### 6.2 规划边界

以下尚未由当前工具提供：readiness 中的结构化 endpoint/QoS gate、generic serialized Topic Observer、自动 QoS 推断、LTTng/ros2_tracing、perf/pcap 编排、多跳因果恢复和两机 Agent。业务消息没有序号、时间戳或因果 ID 时，工具不会声称能够恢复精确 E2E。

任意 topic 的通用通信时延是核心需求，但不能通过时间最近邻或额外 subscriber 推断为目标业务 E2E。待审核方案采用类型无关 trace、RMW/Driver 原生关联和统一证据控制面；当前 DDS 同时具备 reader-level `stage_only` 与选定 capability 的独立 exact artifact。选定 capability 的 clock/topology/trace-quality 已绑定在 validated source/formal chain 中并完成 formal P0a completion，但 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`；`METRIC_AVAILABLE` 仍是 metric-axis failure。通用 exact、异步/multi-handler/multi-driver、其它类型/transport/ROS2 formal exact 仍未实现，不能作性能接受、排名或 latency advantage 声明。

## 7. IBMW 采集工具设计

### 7.1 External P0

IBMW Adapter 从外部只读采集：

- IBMW version/git identity 与 source root；
- CMake executable、cache hash、构建目录与构建选项；
- 指定 Transport YAML 的关键配置摘要；
- SUT 和 artifact binary 的路径、device/inode、大小、mtime 与 SHA-256；
- 运行期 `/proc/<pid>/exe` 和 maps 中实际加载 image 的身份；maps 只接受 permissions 含 `x` 的实际可执行映射；
- Core Metrics capability 和 artifact 状态。

required provenance 要求非空 `scenario_contract`、配置过的 build directory、实际 SUT executable、artifact binaries 和非空 runtime image declarations。运行后 `validate` 会重新读取这些身份；源码、二进制或 CMake cache 变化均 fail-closed。runtime match 绑定采样 PID/start ticks、device/inode、路径、大小和 artifact SHA-256，只读 data mmap 不授予 image 资格。

本设计的运行目录用于检测非恶意篡改、缺失、陈旧、内部不一致和采集后环境变化。目录内 hash 不是数字签名，不抵抗拥有整目录写权限的攻击者离线重写采样、evidence、manifest 和 hash。主动防伪属于目录外密钥签名、远端证明或不可变存储的后续安全能力。

### 7.2 Core Metrics artifact 消费

采集器在启动前记录 `metrics.jsonl` 基线，managed SUT 结束后只归档本轮变化的文件到 `raw/core_metrics.jsonl`。状态为：

- `supported`：合同、内容、身份和受控 finalization 均有效；
- `unsupported`：未配置或未产生可用 artifact，External P0 可继续；
- `stale`：内容与运行前相同；
- `partial`：仅可诊断，例如 attached optional；
- `invalid`：格式、来源或 finalization 不满足合同。

JSONL 每行必须是完整对象，使用 `ibmw.core_metrics.v1`，`snapshot_seq` 严格递增，包含非空 `kind`、`cumulative` 对象和 `complete=true`。

## 8. IBMW 本体适配设计

### 8.1 当前已实现

Core Metrics V1 支持进程启动时的 `off|counters`：

```text
IBMW_CORE_METRICS=off|counters
IBMW_OBSERVABILITY_OUTPUT_DIR=<directory>
```

当前实现包括 Engine-owned Runtime/Registry、普通发送、多 Driver、user/POD loan、direct iceoryx loan、非 retained 与 retained receive 生命周期切片、Reporter V1 边界，以及 Engine 关闭时的 `metrics.jsonl` final snapshot。当前 DDS P0a managed binary trace 已提供 `publish_write`、`take_receive`、`dispatch` 三类 stage 事件，并具备独立 exact callback artifact；exact 仅限受控 synchronous single-handler capability，不能外推为通用 exact。关闭态优先返回；计数数据用于内部归因。

已有功能闭环不等于任意 capability 的性能门禁通过。Core Metrics formal host E2E 已通过 sealed trials、质量门和预注册 verdict，历史 `FAIL`/`INCONCLUSIVE` 不再作为当前门结论；该结果只适用于既定 host capability/合同，不外推到 OHOS managed P0a 或其它 capability。旧 DDS stage trace 不能替代 callback correlation；选定 capability 的 collector-owned resolution、identity trace、semantic hook callgraph、normalized events、latency samples、communication latency integrity 和 delivery evidence 已在 system DDS、Fast DDS 2.6.12 与 3.2.2 通过。v8 exact source 保持 candidate-only/P0A_ARTIFACT 且不可变；v9 formal snapshot 替换重叠 trace_quality/fault/rollback 文档并新增 `formal_anchor`，不回写 v8。campaign v2 的四个 public role 为 v9 formal exact 和三个 v8 companion，显式保留 v8 source，并验证五个 content-addressed object、trusted contracts、complete declaration closure、exact payload set、archive-derived specialized semantics 及目录外 expected root。该 formal chain 绑定 selected capability 的 clock/topology/trace-quality 事实并完成 formal P0a completion，状态为 `valid_for_p0a_completion=true`；metric 仍为 unsupported/invalid，不输出统计性能结论。

当前 formal campaign 为 v2，四个 public role 为 v9 formal exact 与三个 v8 companion；v8 exact source 明确保留为不可变 candidate-only/P0A_ARTIFACT，v9 formal snapshot 替换重叠 trace_quality/fault/rollback 文档并新增 `formal_anchor`。campaign v2 使用 deterministic content-addressed tar object，验证五个 content-addressed object、trusted contracts、complete declaration closure、exact payload set、archive-derived specialized semantics 和跨 role anchor/contract/build identity 闭包；最终验收使用目录外 `expected_campaign_root_sha256`。rollback fresh build 可复用经内容身份校验的外部 dependency source，但不复用其 build object；两个产品 target 的 link plan、response file、object/archive input 与最终 image 均被封存并由 archive validator 重验，IBMW object/archive 只允许来自 fresh private build root。其 threat model 为 `cooperative_contained_sut_no_active_same_uid`，不提供 active same-UID resistance。该链在三套 DDS 运行时通过，并完成 selected capability 的 formal campaign-to-P0a closure；metric 轴仍为 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`，不得宣称性能接受、排名或 latency advantage。

### 8.2 指标语义

计数按 logical、source/driver 和 lifecycle 边界分层。`driver send returned` 只表示同步调用返回，`callback completed` 只表示用户 callback 返回，都不等于远端业务完成。多 Driver fan-out 中 logical publish 与 per-driver send 基数不同，不做直接守恒。

字节数、wire bytes、remote ack、retransmit、fallback 或 queue wait 只有在对应路径提供可靠数据时才能输出；当前不具备通用能力的项目保持 `unsupported`。

### 8.3 规划边界

histogram、sampled trace、运行时动态开关、OTLP exporter、通用 queue wait 和所有 Driver 专有网络指标属于后续能力。

## 9. 结果包与校验

当前单轮目录包含：

```text
<output>/<run_id>/
├── manifest.json
├── validation.json
├── report.md
├── environment/
├── topology/
├── capability/
├── raw/
│   ├── process.jsonl
│   ├── network.jsonl
│   ├── latency.jsonl          # 配置且有效时
│   ├── core_metrics.jsonl     # 配置且归档时
│   └── runtime_images.json    # required provenance 配置时
├── normalized/
└── logs/
```

`validation.json` 分别给出 `valid_for_collection`、`valid_for_metric`、`valid_for_comparison`、逐项 `rules` 和 `failure_codes`。`compare` 不信任陈旧 validation：它会重新验证运行目录，并要求磁盘记录与实时结果完全一致。

正式报告必须保留 unsupported、invalid 与失败原因。数值报告至少注明 metric availability、measurement kind、场景合同、资源范围、样本数及来源 artifact。

## 10. 统计与结果解释

规范化 A/B/C 的完整设计应以 block 为单位：一个 block 含相同合同下的 A、B、C 各一次；任一 lane invalid 时该 block 不进入配对比较。当前版本化 `comparison-plan` 已重验运行目录、block/lane identity、contract/resource/workload/order 元数据、run/path 唯一性和 A/B/C 集合完整性；它仍不计算完整配对统计。每轮独立统计和 B/A、C/A、C/B 属于后续正式验收职责。

探索性结果只能用于发现趋势和验证工具链。正式声明还需要预注册的样本量、阈值、异常处理、置信区间和目标平台验收。非等价 C 只展示带能力标签的 Pareto 结果，不生成无条件“优于 DDS”的结论。

## 11. 通用性与约束

- 当前 V1 面向 Linux 单机进程，可用于 ROS 2 Humble/Fast DDS 基线，也可在能力明确时用于其他 ROS/RMW 环境；未验证组合不能外推。
- PSS 依赖 `/proc/<pid>/smaps_rollup`，不可读时为 `null`；RSS 仅作诊断。
- 网络数据是指定接口计数器，不等于 topic wire bytes。
- ROS Graph 是 CLI 快照，不保证捕获瞬时 endpoint。
- collector 不构建 SUT、不修改 IBMW 配置、不自动选择公平场景。
- required artifact 和 provenance 采取 fail-closed；optional 降级不会被包装成有效指标。

## 12. 首版明确不做

- 通用 `LD_PRELOAD` DDS hook；
- 自动推断多跳消息因果；
- 永久全量 pcap 或 scheduler trace；
- 每消息 OpenTelemetry span；
- 跨网络、SHM、intra-process 统一 wire bandwidth；
- 自动把非 DDS Transport 声明为 DDS 语义等价；
- 自动生成正式产品性能声明。
