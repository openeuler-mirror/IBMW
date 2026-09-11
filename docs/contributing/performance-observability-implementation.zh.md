<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# 性能采集与可观测性实施说明

本文面向维护者，记录可长期维护的实现结构、交付边界和验收要求。设计口径见[性能可观测性设计](../architecture/performance-observability-design.zh.md)，用户命令见[性能采集指南](../guides/performance-collection.zh.md)。临时运行目录、阶段性测试数量、一次性 evidence hash 和自动生成合同不属于本文档。

## 1. 交付原则

实施固定按三条主线组织：

1. **ROS2 采集工具**；
2. **IBMW 采集工具**；
3. **IBMW 本体适配**。

A/B/C benchmark 是三条主线的共同集成场景。公共外部指标负责比较，IBMW Core Metrics 负责归因，两类数据不得互相替代。

所有新增能力必须同时满足：

- 不修改被测业务逻辑；
- 未支持值使用 `unsupported`，未知值不写成 0；
- artifact 有 schema、运行身份、完整性和 provenance；
- managed/attached 生命周期语义不混淆；
- required 配置 fail-closed，optional 配置明确降级；
- 设计项、功能闭环和性能验收分别标注。

## 2. ROS2 采集工具

### 2.1 稳定交付

ROS2 Adapter 当前稳定职责为：

- 从环境读取 ROS distro、RMW、domain、security、discovery 和 AMENT 信息；
- 通过 ROS 2 CLI 被动采集 node/topic；当前 readiness 快照不包含结构化 topic info/QoS gate；
- 在共享 deadline 内轮询 topic readiness；
- 在 Graph 查询前后校验 SUT PID 与 Linux start ticks；
- 使用公共采集内核记录进程树、CPU、PSS/RSS、线程和网络；
- 归档并严格验证 producer-owned latency JSONL；
- ROS 不可用时返回 degraded/unsupported。

ROS2 Adapter 不导入 rclpy，也不创建额外 subscription。这个约束防止工具静默改变 DDS matching 和被测系统负载。

### 2.2 Producer-owned latency 合同

managed collector 向 SUT 注入：

```text
MW_PERF_RUN_ID=<run_id>
MW_PERF_ARTIFACT_SCHEMA_VERSION=mw_perf.latency.v1
```

producer 写出的每条数据必须只含 `schema`、`seq`、`send_ns`、`receive_ns`、`latency_ns`、`topic`、`type`、`payload_bytes`、`valid`。最后一条 completion 必须只含合同规定字段，并绑定 run ID、producer PID/start ticks、记录数、topic/type 和 `complete=true`。

实现必须拒绝：不完整末行、空行、非有限 JSON 数值、非递增 seq、时间公式错误、混合 topic/type、缺失/重复 completion、身份不一致和归档时内容变化。

### 2.3 后续边界

generic Topic Observer、LTTng/perf/pcap、多机 Agent 和自动多跳关联仍是后续工作。实现这些能力时必须新增 capability 和扰动标记，不能改变现有 P0 的被动语义。

## 3. IBMW 采集工具

### 3.1 公共生命周期与资源采集

共同内核提供：

- `doctor/run/validate/report/compare` CLI；
- managed 新 session 和 attached PID 模式；
- PID/start ticks 与后代进程跟踪；
- TERM 后有界 KILL 的 managed cleanup；
- `/proc` CPU、PSS、RSS、线程采样；
- sysfs network counter 采样；
- manifest、validation、report 和分层 artifact 目录。

`compare` 对旧 `runs` plan 保持最小聚合兼容；对 `mw_perf.comparison-plan.v1` 会 fail-closed 检查每个 block 的 A/B/C 完整性、block/lane identity、contract/profile/workload/resource scope/order 一致性、run/path 唯一性和 validation 新鲜度。它仍不执行完整 benchmark 统计或发布正式胜负结论。

### 3.2 IBMW External Adapter

External Adapter 保持只读，不构建、启动或修改 IBMW。它采集：

- 版本或 git commit；
- CMake 可用性、cache、build options 和 hash；
- Transport 配置摘要；
- Core Metrics capability；
- source、binary、runtime image provenance。

required provenance 的稳定规则：

- `scenario_contract` 非空并保存 canonical hash；
- `build_dir/CMakeCache.txt` 必须存在；
- wrapper 不能冒充 SUT，必须声明实际 executable；
- 至少声明一项 artifact binary；
- required provenance 必须有非空 runtime image declarations，验证器不允许通过删除 manifest 诊断字段关闭门禁；
- 运行期 image 按 `/proc/<pid>/exe` 或 maps 与声明匹配；maps 只接受 permissions 含 `x` 的实际可执行映射，并绑定 PID/start ticks、device/inode、路径、大小和 artifact SHA-256；
- `validate` 时重新计算 source、binary 与 cache identity。

上述 fail-closed 语义针对缺失、陈旧、内部不一致和采集后环境变化。运行目录是自包含证据包，不是防攻击者篡改的信任锚：拥有整目录写权限的主体可以同时重写采样、evidence、manifest 和无密钥 hash。若验收要求主动防伪，必须另行设计目录外密钥签名、远端证明或不可变存储，不能只增加更多目录内 hash。

### 3.3 Core Metrics 归档

采集器在运行前保存 metrics 文件 metadata/hash，managed 停止完成后再读取 final 文件。只有本轮变化、严格 JSONL 有效、source/archive hash 相等且优雅 finalization 成立时才为 `supported`。

attached 模式不停止目标，因此无法证明 final snapshot。optional artifact 可以归档为诊断数据；required artifact 必须失败。已有旧文件内容未变化时标为 `stale`。

### 3.4 后续边界

多机 Controller/Agent、自动 workload 调度、完整 block ratio 统计与趋势平台未纳入当前稳定交付。

## 4. IBMW 本体适配

### 4.1 Runtime 与开关

Core Metrics 由 Engine 生命周期拥有。运行开关为：

```text
IBMW_CORE_METRICS=off|counters
IBMW_OBSERVABILITY_OUTPUT_DIR=<directory>
```

`off` 是默认值。关闭路径应尽早返回，不读取时钟、不写文件，也不调用 exporter。`counters` 只用于已注册的固定路径计数，Engine 正常关闭时以原子替换方式生成 `metrics.jsonl`。

### 4.2 已接入路径

当前功能实现覆盖：

- ordinary logical publish 与 per-driver send；
- multi-driver 分层计数；
- user loan 与 POD loan；
- direct iceoryx loan；
- 非 retained `AttachLoaned`；
- retained receive/release 生命周期；
- Reporter V1 host/extension 边界；
- shutdown final snapshot。

`comm_trace` 属于本主线的独立交付边界：当前提供默认关闭的 core hook、DDS writer/reader identity trace、同步 callback registration identity scope、独立 exact record/query、managed `.ibce` exporter 和 stage sink；旧 V1 DDS dispatch 仍保持 `stage_only`，受控 exact artifact 只有在 capability、managed finalization、native round-trip 和正式 P0a 门禁全部通过后才能登记 `exact_correlated`。comm_trace 与 Core Metrics 解耦，Core Metrics 功能和 artifact 已闭环；性能门是独立验收轴，不作为主要功能闭环依据，也不外推到 OHOS managed P0a、metric axis 或其它 capability。仍不能用 Core Metrics 做 ROS2/IBMW 排名。

指标名称与 kind 以实际 `ibmw.core_metrics.v1` artifact 为准。不同路径的 attempted、succeeded、failed、publish、delivery、callback 和 release 语义必须独立，不能为凑守恒关系合并。

### 4.3 生命周期与 ABI 约束

- Runtime/Registry 生命周期覆盖所有 Reporter 与 Slot；
- receive disable、callback drain 与 retained release 必须在线性化边界内完成；
- final snapshot 在相关 Driver/Slot 仍有效时生成；
- Reporter 使用版本化 sidecar，不修改冻结的 Transport Driver vtable/capability 布局；
- 跨扩展报告不得阻塞业务线程或反向调用业务代码。

### 4.4 性能边界

功能、artifact 合同和安装后 ABI 闭环已经建立。性能门继续按既定 host capability/合同独立验收，不作为功能状态依据，也不得外推为 OHOS managed P0a 或其它未验收 capability；任何性能结论必须保持预注册、质量有效、不可放宽。

- 不得宣称 counters 可用于 ROS 2/IBMW 胜负排名；
- 不得将 host 结果外推为 OHOS managed P0a 或其它未验收 capability；
- 不得通过放宽阈值、忽略边界丢包或使用描述性吞吐替代预注册 verdict。

### 4.5 后续边界

histogram、sampled trace、动态控制、OTLP、通用 queue wait 以及 wire bytes/retransmit/remote ack 等 Driver 专有指标后置。能力缺失时必须保持 `unsupported`。

### 4.6 OHOS remote exact candidate 边界

OHOS remote candidate 使用设备本地 native agent 管理 production DDS exact candidate：agent 在设备私有目录内创建 trace FD、执行并约束单个 SUT 生命周期、稳定扫描 producer 文件并发布 canonical USTAR；HDC 只负责把 archive、单行 result JSON 和对应 request JSON 传到 host。host 安装态 `import-ohos-capture` 重新校验 request challenge、archive/tree hash、PID/start ticks、meta/seal 和 stage/exact 语义。该结构解决了 host FD 不能跨 HDC 传递的问题，但不把 HDC 当作可信 FD 或生命周期通道。

该能力当前只登记 remote candidate：已完成物理验证的 legacy import 可证明本次设备 capture 的受控生命周期、snapshot 完整性和 `exact_correlated` producer 语义，但不生成 collector `directory_finalization.json`。仓内另有预注册哈希、双阶段 runtime image 观察、host candidate materialization 和 fault evidence 的后续候选实现；这些能力尚未完成物理 formal campaign/rollback 验收，不能据此升级既有真机证据。所有 remote candidate 固定 `formal_p0a=false`，`metric_availability=unsupported`，`valid_for_metric=false`，`valid_for_comparison=false`，不得形成 OHOS 性能结论。OHOS formal P0a 是独立 capability，不阻塞三条主线的主要功能闭环。构建、部署和使用入口见[性能采集指南](../guides/performance-collection.zh.md)。

## 5. 跨主线集成

### 5.1 最小场景合同

集成场景至少固定：

- A/B/C solution 与兼容等级；
- 单机/跨机、同进程/跨进程边界；
- topic/type、payload、QoS 和序列化；
- offered load、warmup、measured count 和 timeout；
- completion、丢失、重复、乱序及 payload correctness；
- 纳入 solution 与 harness 的进程资源。

合同 canonical JSON 的 SHA-256 写入每个 run manifest。不同 hash 的结果不比较。

### 5.2 A/B/C 原型边界

仓库 `src/samples/cpp/transport_benchmark/abc_interop/` 提供独立 runner：

- A：ROS 2 Humble endpoint；
- B：IBMW DDS ROS 2-compatible endpoint；
- C：IBMW iceoryx ordinary `Send()` endpoint。

C 没有 DDS 协议级 reliability/discovery 语义，必须保留 semantic warning。runner 的固定 CSV、subscriber status、C publisher completion 和 freshness gate 是 benchmark 专用合同，不是通用 collector 自动推断能力。

### 5.3 结果有效性

只有以下项目同时成立时才可进入比较：

- run completed，采样文件结构完整；
- adapter readiness 与指标可用性通过；
- required latency/Core Metrics/provenance 全部通过；
- scenario contract、resource profile、block/order 元数据一致；
- correctness 与 workload 条件满足预注册规则。

失败运行和原始 artifact 应保留，但不得进入聚合。

## 6. 变更验证

文档或工具合同变更至少检查：

```bash
./install/bin/mw-perf-collect doctor --adapter ros2 --config doctor.json
./install/bin/mw-perf-collect doctor --adapter ibmw --config doctor.json
python3 -m pytest tools/mw_perf_collect
python3 -m unittest discover \
  -s src/samples/cpp/transport_benchmark/abc_interop \
  -p 'test_run_block.py'
# DDS C++ focused test（启用 comm_trace 的受控构建）
ctest --test-dir <受控构建目录> -R 'ibmw_dds_comm_trace_test'
# OFF symbol gate（关闭 comm_trace 的受控构建）
ctest --test-dir <关闭 comm_trace 的受控构建目录> -R 'comm_trace_compile_out_test'
git diff --check
```

安装后两条 doctor 命令是最小双适配器 smoke：它们必须输出可解析 JSON，且顶层
`status` 属于 `ready`、`degraded` 或 `unsupported`。测试使用最小配置，不启动
ROS 2/IBMW daemon；外部环境缺失应通过合同状态降级，而不是导致 import 或模块布局错误。

按实际改动补充 C++ 单元、生命周期、扩展边界和 E2E 测试。测试报告记录命令、环境和 verdict，不在稳定文档中固化容易过期的累计测试数量或临时目录。

## 7. 文档维护规则

- 架构文档说明设计口径和已实现/规划边界；
- 本文记录稳定实现与维护约束；
- 用户指南只给当前可用命令，并明确原型边界；
- 临时进展矩阵、benchmark evidence、自动合同和运行目录不迁入正式 docs；
- 仓内链接使用相对路径，不写开发机绝对路径。
