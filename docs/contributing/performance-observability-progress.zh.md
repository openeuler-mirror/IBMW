<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# 性能可观测性当前进展矩阵

本文记录当前正式实现和验证边界。状态只使用“功能闭环”“验证中”“阻塞”，不使用易误导的百分比或临时测试累计数字。

## 总目标与共同场景

总目标是为 ROS2 与 IBMW 建立低侵入、可复现、可审计的性能证据，并在事件边界、资源范围、正确性、时钟和场景合同一致时支持正式比较。A/B/C 是三条主线共同的集成场景，不是第四条主线：

- A：ROS2 -> 当前 RMW/DDS；
- B：IBMW -> DDS ROS2 兼容路径；
- C：IBMW -> 新 Transport。

A/B/C 已完成一轮 collector-managed 的 P0 comparison gate；各运行目录均通过生命周期、资源范围、artifact freshness/hash、lane identity 和正确性门禁。当前 `compare` 只证明这些运行目录具备可比性，不执行聚合统计或胜负判定，因此仍不能据此发布性能排名或显著性结论。

## 主线矩阵

### 1. ROS2 采集工具

**状态：功能闭环**

**已完成**

- ROS 环境、Graph、readiness、进程资源和网络等被动采集；
- 安装态 clean collector 可从 canonical `ros2_prefix/bin/ros2` 的可信 shebang 推导 Python interpreter、包路径和共享库路径；不继承 SUT 或调用方的 `PYTHONPATH`、`PYTHONHOME`、`LD_PRELOAD`、`LD_LIBRARY_PATH`，Graph 查询统一使用 `--no-daemon` 直接发现；
- producer-owned latency JSONL 的身份注入、严格校验、归档和报告；completion 已增加 artifact-level `clock_domain`，当前唯一 supported 值为 `producer_monotonic`，并与 archive、provenance、manifest 绑定且 fail-closed；
- legacy、unknown 或 invalid clock domain 的已归档 artifact 只可用于诊断，强制 `valid_for_metric=false`、`valid_for_comparison=false`，required artifact 使 run invalid；没有 latency artifact 的 optional run 不受影响；
- managed/attached 生命周期和 fail-closed artifact 校验；安装 launcher 隔离源码路径，并验证所加载的是安装 prefix 中的包。
- 已在真实 ROS 2 Humble 安装环境完成安装态 doctor，状态为 ready；真实 managed A-lane E2E 归档 3000 条 `producer_monotonic` latency，readiness、进程/网络资源采集、受控 finalization 和最终 validation 均通过。

**进行中**

- 统一证据合同、A/B/C 集成场景和聚合统计方法的验证；
- generic observer 和 ROS2/RMW 到最终 rclcpp callback 的版本锁定 identity 穿透验证。

**阻塞/未完成**

- producer-owned latency 已实现，但 generic observer 尚未实现；
- 通用 exact E2E 尚未实现，不能从时间最近邻、额外 subscriber 或 stock tracing 推断目标 callback 时延；
- artifact-level `producer_monotonic` 只声明 producer 自报样本的单调时钟域和归档合同，不证明 generic exact callback、SampleKey/DeliveryKey、跨主机时钟同步或 producer 诚实性；已验证的真实 A-lane 仍是受控 producer-owned 合同，不是通用 ROS2/RMW exact observer；
- Fast DDS 2.6.12 / Fast CDR 1.0.29 与 Fast DDS 3.2.2 / Fast CDR 2.3.0 均已完成精确 source-prefix、完整 DDS extension 编译和 native identity round-trip；SYSTEM Fast DDS 2.6.11 仅作为兼容回归。

**完成判据**

完成通用能力所需的 hook、SampleKey/DeliveryKey、callback identity、版本/运行库 provenance、trace loss、时钟、拓扑和 finalization 证据，并通过预注册性能门和正式 P0a/P2 验收。producer-owned 路径本身只以其受控业务合同作为闭环，不等同于通用 exact。

### 2. IBMW 采集工具

**状态：功能闭环**

**已完成**

- 源码 collector 的 `doctor/run/validate/report/compare` 主流程、managed/attached 生命周期、资源采集、provenance 和 artifact 校验；
- P0a managed DDS binary trace：collector sink、固定格式 binary segment、meta/seal、目录 finalization 和 fail-closed 校验；
- collector 安装包布局和安装后运行路径已修补，`json_strict.py` 已纳入安装清单，并通过独立安装 smoke；
- `tools/mw_perf_collect/tests/test_cli.py` 已增加安装后双适配器 smoke：隔离 `PYTHONPATH`/`PYTHONHOME`，运行安装后的 `mw-perf-collect doctor --adapter ros2` 和 `doctor --adapter ibmw`，验证 JSON、adapter 以及 `ready`/`degraded`/`unsupported` 状态；同时验证真实交付布局；
- 新增安装后 CLI 矩阵 smoke：隔离 `PYTHONPATH`/`PYTHONHOME`，验证安装后的 `validate`/`report`/`compare`，检查 JSON/Markdown 产物、`schema_audit`、业务失败退出码和 `failure_codes`；使用现有 fixture，不伪造性能 PASS；
- IBMW doctor 已补充顶层 `adapter`/`status`，Core Metrics 不可用时可正常降级；
- schema audit 增量交付：`core.py` 记录 result/validation/comparison-plan schema 的 `$id`、相对文件路径、原始 bytes SHA-256、validator/version，`validate`/`report`/`compare` 产物具备 schema identity；未引入 `jsonschema`，未修改既有 P0a contracts，并保持旧产物兼容；
- 安装资源 schema/contract SHA-256 registry 已交付：安装包记录运行时 schema 与 contract 的内容身份，`validate`/`report`/`compare` 在 registry 缺失或内容被篡改时均 fail-closed；新增有效 run 安装后篡改/删除测试及 registry 集合测试，历史 validation 仅有限兼容缺失的 `schema_audit`/`resource_integrity` 元数据，不放宽当前安装资源校验；
- 当前工具测试通过，验证覆盖 Python collector、DDS trace parser/validator 和相关 artifact 合同。
- 已在真实安装布局完成 B-lane managed E2E：3000/3000，丢失、重复、顺序、payload 和 timestamp error 均为 0；进程/网络 artifact、最终 validation 以及 comparison gate 均通过。该 endpoint 在进程内嵌入 IBMW Engine，不应把此结果描述为独立 IBMW daemon E2E。
- 已安装并验证 `mw-perf-collect import-ohos-capture`；host importer 对 canonical USTAR、request challenge、archive/tree hash、child PID/start ticks 绑定及 producer trace semantic import 严格 fail-closed。
- 安装态真实 native managed P0a qualification 已通过：在隔离源码树 Python import 的环境中由安装后的 `mw-perf-collect` CLI 运行 production `ibmw_dds_managed_exact_candidate`，并确认已加载的 collector 模块及 schema/contract 资源均位于安装 prefix；同一既有 campaign 流程完成 P0a validation、fault/rollback companion、v9 formal anchor 和 formal campaign replay。该结论仅覆盖 host 选定 capability `dds.ordinary.nonplain.single_driver.sync_callback`，保持 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`。

**持续验证**

- managed DDS binary trace 继续在受控构建、运行库和正式证据合同上执行回归；该持续验证不表示功能闭环仍有缺口。
- 完整安装遵循 build→codegen→install；裸 `cmake --install` 不支持替代所需的 codegen build。

**能力边界**

- 安装后 CLI fixture、真实 A/B managed E2E 与安装态真实 native managed P0a qualification 是不同证据类别，不能互相替代；A/B endpoint 可在各自进程内嵌入 Engine，不能描述为独立 daemon E2E。
- 选定 host capability `dds.ordinary.nonplain.single_driver.sync_callback` 的 completion axis 为 `valid_for_p0a_completion=true`；metric/comparison 轴保持 unsupported/false，不代表性能通过或胜负结论。
- schema audit 只提供 provenance 与合同审计；历史 validation 仅兼容缺失的 `schema_audit`/`resource_integrity` 元数据，不放宽当前资源完整性或语义校验。
- OHOS formal P0a、通用 exact、其它 transport/type、异步、多 handler 和多 driver 按独立 capability 验收，不属于本主线已闭环的 host collector capability。

**完成判据**

安装后 CLI 已从安装 prefix 加载所需包内运行时模块与资源；managed P0a stage/exact artifact 的身份、拓扑、seal、loss、目录 finalization 和 manifest 合同验证通过；受控 host capability 的真实 native round-trip、完整 E2E 因果链和正式 P0a 验收已通过并登记 `exact_correlated`。该完成判据不扩展到通用 exact、OHOS formal P0a 或 metric/comparison 轴。

### 3. IBMW 本体适配

**状态：验证中**

**已完成**

- Core Metrics V1、Runtime/Registry 生命周期、普通发送、多 Driver、loan/retained 路径切片和 final snapshot；
- `comm_trace` core hook、DDS 插件 trace 和默认关闭/compile-out 结构；
- DDS P0a managed binary trace 的 `publish_write`、`take_receive`、`dispatch` stage 事件，以及独立 final callback exact record；
- option A fixed-width internal metadata 已实现：32-byte publish envelope 和 40-byte callback invocation envelope，不修改业务源码/二进制、IDL、payload、`MessageFrame` 或 frozen public ABI；
- contract v3 collector-owned `resolution.v2` 当前 artifact 已通过真实 managed candidate：collector 在启动前冻结 canonical image 的 FD snapshot SHA-256/device/inode/size，并将其与 managed run 期间至少一次 collector 轮询到的根 SUT executable 和 production DDS extension maps identity 完全匹配；这不证明所有采样时刻持续存在。image resolution publication 要求 graceful managed zero-exit 和 matched observations，不要求 intentional fault trace 本身有效，因此可保留 fault run 的 image attestation 而不改变 invalid trace status。exclusive publication 使同名文件预占导致发布失败和 run invalid；threat model 为 `cooperative_contained_sut_no_active_same_uid`，不抵抗 active same-UID 对手。
- contract v4 collector-owned `identity_trace.v2` 已通过真实 managed candidate：validator 从 sealed stage/exact raw source set 重建每个 DeliveryKey 的 writer/sequence、reader、callback registration ID 与完整时间边界，并执行在线发布和离线逐字节重算。该 artifact 只证明成功 delivery 到 callback invocation 的 identity/因果链，不证明 callback 注册生命周期或未投递 handler。
- contract v5 collector-owned `hook_callgraph.v2` 已通过真实 managed candidate：它将可信固定 catalog 中的四个逻辑边界与 runtime image identity、raw record counts、DeliveryKey set 和时间顺序绑定，并支持离线逐字节重算。该 artifact 证明 semantic boundary chain，不声称动态调用栈、运行期函数地址或 source-to-build provenance。
- contract v6 collector-owned `comm_event.v2` 与 `latency_sample.v2` 已通过真实 managed candidate：每个 SampleKey 一条 publish span、每个 DeliveryKey 三条下游 span和一条 `callback_start - publish_start` 样本，全部绑定 identity/resolution/hook input hashes 并支持离线逐字节重算。它们是成功 exact delivery 的结果投影，不替代 delivery、fault 或 rollback proof。
- contract v7 collector-owned `communication_latency.v2` 已通过真实 managed candidate：它冻结 metric boundary、公式、时钟域、完整 input hashes 和 DeliveryKey sample set，但不输出未预注册统计；其 `pass` 只表示 run-scoped artifact integrity。v7 强制 candidate-only、metric unsupported 和 comparison-invalid。
- v9 formal snapshot 已替换重叠的 trace_quality/fault/rollback 文档，并新增 `formal_anchor`；v8 exact source 保持 `candidate_only=true`、`P0A_ARTIFACT`，作为不可变候选源且从不被修改。campaign v2 有四个 public role：v9 formal exact 与三个 v8 companion，并显式保留 v8 source；它验证五个 content-addressed object、trusted contracts、complete declaration closure、exact payload set、archive-derived specialized semantics 和目录外 `expected_campaign_root_sha256`。rollback fresh build 仅复用经内容身份校验的外部 GoogleTest source，不复用其 build object；两个被审计产品 target 的 link plan、response file、object/archive input 和最终 image 均进入独立 archive validation，IBMW object/archive 必须来自 fresh private build root。该链在 system DDS、Fast DDS 2.6.12 与 3.2.2 通过，campaign threat model 为 `cooperative_contained_sut_no_active_same_uid`，无 active same-UID resistance。
- Core Metrics archive/finalization contract 的仓内测试闭环已增强：覆盖实际 `archive_core_metrics→manifest/provenance→validate_dir` happy path、source/archive hash、`record_count`/`kinds`、`graceful_finalization`/`final_snapshot`、零字节 JSONL、篡改时 fail-closed，以及完整 artifact 仍保持 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`；unknown kind 的 forward-compatible 行为保持不变。
- HDC 仍不能跨主机传递 collector-owned inherited FD，但设备本地 native lifecycle/snapshot agent 已在 OHOS 真机关闭 inherited-FD 注入、export 和 snapshot 可行性：agent 在设备端创建私有目录、向子进程传递本地 trace FD、约束生命周期并发布规范化 archive，再由 host 严格导入。
- production exact candidate 的 OHOS 真机运行完成 1000 次 publish、receive、dispatch，共 3000 条 stage record，并完成 1000 条 exact record；19 个 segment 通过零丢失语义校验并登记 `exact_correlated`。archive 与 request challenge/hash、child PID/start ticks 完整绑定。
- 真机集成暴露并修复了跨 DSO RTTI identity 问题：`TransportDriver` 与 comm-trace overlay interface 改为 default-visible，并增加导出符号 audit，避免主程序与 DDS extension 形成不同 RTTI identity。

**进行中**

- comm_trace 对选定 capability 的 clock、topology、trace-quality 事实已绑定进 validated source/formal chain；仍需保留 metric 轴状态（`metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`）。v8 exact source 仍不可变、candidate-only/P0A_ARTIFACT，v9 formal snapshot 与 campaign v2 不回写它；
- Core Metrics formal host E2E 性能门已通过 sealed trials、质量和预注册 verdict；因此 host performance gate 已关闭，历史失败轮次不再作为当前门结论。稳定文档不固化临时目录或阶段性数值；
- 新 round9 micro controls 的守恒与 paired bootstrap projection allowance 均为 `PASS`；该结果只用于解释 hot hook CPU 成本，不替代 E2E 或 latency gate；
- OHOS AArch64 真机的 Core Metrics direct-loan counters 路径已验证；独立 device paired runs 只作诊断，不替代 managed P0a，也不形成性能结论；
- remote OHOS candidate importer 显式输出 `formal_p0a=false`，不创建 `directory_finalization.json`，且没有新增 trusted remote contract、formal campaign、runtime image attestation、fault 或 rollback evidence；其 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`。仍需正式 OHOS P0a、通用 exact、异步/multi-handler/multi-driver、其它 transport/type/ROS2 formal exact。

**阻塞/未完成**

- host 选定 capability 的 v9 formal snapshot 与 campaign v2 已完成 formal campaign-to-P0a closure，并在 system DDS、Fast DDS 2.6.12 与 3.2.2 通过，状态为 `valid_for_p0a_completion=true`。OHOS remote candidate 是独立边界，保持 `formal_p0a=false`，没有 directory finalization、formal campaign、runtime image attestation、fault 或 rollback 闭环。这不改变 metric 轴：`metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`；不得据此宣称性能接受、排名、latency advantage 或通用能力；
- 通用 exact callback 和 A/B/C 聚合统计/胜负判定仍未完成；collector-managed P0 comparison gate 通过不代表这些能力已经闭环。

**完成判据**

Core Metrics 已通过正式性能门，但仍需完整 artifact/finalization，并按各独立 capability 完成验收；comm_trace 对选定 capability 已完成 identity、故障/丢失、拓扑、时钟和正式 P0a completion 轴，登记 `valid_for_p0a_completion=true`。metric 轴仍为 unsupported/invalid，不作性能接受、排名或 latency advantage 声明；通用能力继续按独立 capability 验收。

### 问题反馈与决策

- **问题**：旧文档沿用历史 round8/lossless-v2 `FAIL`。
- **反馈**：新的 sealed formal evidence 为 `PASS`。
- **决策**：解除 Core Metrics 性能阻塞，但不扩大 capability，也不宣称 metric、comparison 或 performance ranking。
- **问题**：新增 schema provenance 导致旧封存 validation 在 runtime fault/campaign 校验中误判。
- **反馈**：定位为比较逻辑未兼容缺失的 `schema_audit`，而非外部根校验问题。
- **决策**：仅忽略缺失的 `schema_audit` provenance，继续严格比较所有语义字段，不放宽外部根校验。
- **问题**：安装态只覆盖 doctor/qualification，`validate`/`report`/`compare` 尚未形成矩阵保护。
- **反馈**：仓内 fixture 可以验证安装后的命令加载和产物合同。
- **决策**：补充安装态 CLI 矩阵，但不把 fixture smoke 升级为真实 endpoint E2E 或性能结论。
- **问题**：安装后的 schema/contract 资源若缺失或被篡改，可能使 `validate`/`report`/`compare` 继续使用不可信资源。
- **反馈**：有效 run 的安装后篡改/删除场景与 registry 集合测试已复现该边界；历史 validation 只携带有限的 `schema_audit`/`resource_integrity` 元数据兼容需求。
- **决策**：建立安装资源 SHA-256 registry，并对 registry 缺失或内容不匹配统一 fail-closed；仅兼容历史 validation 中缺失的上述元数据，不以兼容名义放宽当前资源完整性或语义校验。
- **问题**：Core Metrics archive/finalization 的 manifest、provenance 和目录校验边界此前缺少覆盖实际归档链路的合同测试，容易遗漏空文件、计数/类型或篡改后的 fail-closed 语义。
- **反馈**：本轮通过实际 `archive_core_metrics→manifest/provenance→validate_dir` happy path 及 source/archive hash、`record_count`/`kinds`、graceful finalization/final snapshot、零字节 JSONL 和篡改场景测试，确认完整 artifact 的 metric 轴仍为 unsupported/invalid，unknown kind 仍 forward-compatible。
- **决策**：将上述 archive/finalization contract 测试作为第三主线的窄交付记录；它只验证仓内合同，不替代正式 OHOS P0a。后续真机 remote candidate 已关闭设备本地 export/snapshot 可行性，但不改变第三主线“验证中”状态。
- **问题**：producer-owned ROS2 latency completion 过去未在 artifact 层明确时钟域，归档链可能无法区分受支持时钟、legacy/unknown/invalid 值，也需要防止安装 launcher 从源码树误加载包。
- **反馈**：本轮为 completion 增加 artifact-level `clock_domain`，仅支持 `producer_monotonic`，并将其绑定到 archive/provenance/manifest；legacy/unknown/invalid 已归档 artifact 仅保留诊断用途，metric/comparison 均为 false，required 情况 invalid，而无 artifact 的 optional run 保持既有行为。安装 component/CLI smoke 同时确认 launcher 隔离源码路径并从安装 prefix 加载包。
- **决策**：对 completion/archive/provenance/manifest 的 clock-domain 缺失、不一致或非法值统一 fail-closed；不把该窄交付解释为 generic exact callback、SampleKey/DeliveryKey、跨主机同步或 producer 诚实性证明，也不据此升级第一主线状态。真实 A-lane producer-owned E2E 已验证，generic observer 和通用 ROS2/RMW exact 继续列为未完成。
- **问题**：HDC 不能传递 host collector-owned inherited FD，旧口径据此把 OHOS export/snapshot 整体标为阻塞。
- **反馈**：设备本地 native agent 已在物理 OHOS 上完成受控子进程生命周期、本地 FD 注入、snapshot/archive 发布；host importer 已完成 canonical USTAR、challenge/hash、PID/ticks 与 trace semantic 严格校验，production exact candidate 得到 3000 条 stage 和 1000 条 exact record，零丢失且为 `exact_correlated`。
- **决策**：关闭 remote candidate 的 inherited-FD/export/snapshot 可行性问题，但保持 `formal_p0a=false`；不补写 `directory_finalization`，不声称已有 trusted remote contract/campaign、runtime image attestation、fault/rollback、metric、comparison 或性能结论。

### 本轮推进记录

本轮三主线推进如下：

- **ROS2**：第一主线状态不变，仍为功能闭环；安装态 collector 已从 canonical ROS 2 shebang 建立可信 interpreter/prefix 派生，隔离调用方 Python/LD 污染，并以 `--no-daemon` 直接查询 Graph。真实 Humble doctor 为 ready，真实 managed A-lane 完成 3000 条 `producer_monotonic` latency，readiness、资源、finalization 和 validation 均通过。该结果仍是受控 producer-owned E2E，不证明 generic observer 或通用 ROS2 exact。
- **IBMW 采集工具**：第二主线升级为功能闭环；安装态 B-lane managed E2E 完成 3000/3000，且安装态真实 native managed P0a qualification 已在源码树 Python import 隔离环境中通过 production candidate、安装 CLI、P0a validation、v9 formal anchor/campaign replay及安装 prefix 模块/资源校验。该闭环仅覆盖选定 host capability，metric/comparison 保持 unsupported/false；A/B endpoint 可进程内嵌 Engine，不能据此扩展为独立 daemon、通用 exact、OHOS formal P0a 或性能结论。
- **IBMW 本体适配**：第三主线仍为验证中；HDC 不能跨主机传递 inherited FD，但 device-local native agent 已在物理 OHOS 上关闭 lifecycle/FD export/snapshot 可行性，production exact candidate 完成 3000 条 stage、1000 条 exact、19 个 segment 的零丢失 `exact_correlated` 语义导入，并修复跨 DSO RTTI visibility、增加 audit。该候选明确 `formal_p0a=false`，没有 `directory_finalization`、trusted remote contract/campaign、runtime image attestation、fault/rollback 或性能结论；metric/comparison 继续 unsupported/false。

第一主线仍缺 generic observer、通用 exact E2E 和通用最终 callback identity；第二主线已具备安装态真实 A/B managed E2E，并以独立安装态真实 native qualification 关闭选定 host capability 的 managed P0a 发布证据缺口；第三主线 remote OHOS candidate 仍保持 `formal_p0a=false`、`metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`。总目标与 A/B/C 集成场景边界不变。

## 最近验证类别与证据路径

最近验证使用以下稳定命令类别，不在本文固化累计测试数量或临时输出目录：

```text
Python collector focused tests:  PYTHONPATH=tools python3 -m pytest tools/mw_perf_collect
安装 component/CLI smoke：已通过；launcher 隔离源码路径，并验证加载安装 prefix 中的包；不替代真实 endpoint E2E；
安装后 CLI 矩阵 smoke：已覆盖隔离 `PYTHONPATH`/`PYTHONHOME` 下安装后的 `validate`/`report`/`compare`、JSON/Markdown 产物、schema audit、业务失败退出码和 `failure_codes`；使用现有 fixture，不替代真实 endpoint E2E 或 managed P0a；
安装资源 registry 测试：已覆盖有效 run 安装后资源篡改/删除、registry 集合及缺失/不匹配时 `validate`/`report`/`compare` fail-closed；历史 validation 仅有限兼容缺失的 `schema_audit`/`resource_integrity` 元数据；
安装后双适配器 smoke：已覆盖安装后的 `doctor --adapter ros2`/`ibmw`、JSON 合同和 endpoint 不可用时的状态降级；
安装态真实 ROS2 类别：可信 `ros2_prefix`/shebang interpreter、Python/LD 隔离、`--no-daemon` Graph、Humble doctor ready、managed A-lane 3000 latency 及 readiness/resource/finalization/validation；
安装态真实 IBMW 类别：managed B-lane 3000/3000 correctness、process/network artifact、validation 和 comparison gate；endpoint 内嵌 Engine 时不以独立 daemon 作为前提；
OHOS capture/import 类别：native agent lifecycle/snapshot、production exact candidate、跨 DSO RTTI symbol audit、HDC 传输后的 installed `import-ohos-capture` 严格校验；保持 `formal_p0a=false`；
相关 benchmark/runner tests:   python3 -m unittest discover -s src/samples/cpp/transport_benchmark/abc_interop -p 'test_run_block.py'
DDS C++ focused test:           ctest --test-dir <受控构建目录> -R 'ibmw_dds_comm_trace_test'
OFF symbol gate:                ctest --test-dir <关闭 comm_trace 的受控构建目录> -R 'comm_trace_compile_out_test'
文档/变更检查:                  git diff --check
```

对应源码和合同证据路径为：`tools/mw_perf_collect/`、`tools/mw_perf_collect/contracts/`、`src/core/observability/comm_trace.*`、`src/extensions/transport/dds_ext/dds_comm_trace.*`、`src/extensions/transport/dds_ext/verify_no_comm_trace_symbol.cmake` 以及 `src/samples/cpp/transport_benchmark/abc_interop/`。这些路径用于定位可重复验证入口，不代表任何固定测试数量或正式 A/B/C 结论。
