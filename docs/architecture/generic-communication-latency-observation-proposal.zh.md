<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# ROS2 与 IBMW 通用通信时延采集方案

> 状态：待审核设计提案；选定 capability `dds.ordinary.nonplain.single_driver.sync_callback` 的 formal P0a completion axis 已在 system DDS、Fast DDS 2.6.12 和 Fast DDS 3.2.2 通过，状态为 `valid_for_p0a_completion=true`。metric axis 仍为 unsupported/invalid。
>
> 本提案补充[性能可观测性设计基线](performance-observability-design.zh.md)，不替换当前 producer-owned latency 合同。在本提案通过评审、实现并完成性能门禁前，现有工具仍不能声称支持任意 topic 的通用逐消息时延。

> 当前审核决策记录：首个且当前唯一选定 capability 为 `dds.ordinary.nonplain.single_driver.sync_callback`，并采用方案 A：使用版本锁定的 IBMW 内部观测构建和 fixed-width internal envelope/side metadata 传递 identity；业务源码、业务二进制、IDL 和 payload 不修改。该 capability 的 formal P0a completion 已通过，但不扩大为其它 capability，也不代表 metric 或性能验收通过。

## 1. 需求结论

通信时延是 ROS2 与 IBMW 性能对标的核心指标。当前 producer-owned JSONL 需要受控业务主动写入时间戳和序号，只适用于 benchmark 或已改造业务，不能满足以下目标：

- 不修改业务源码和消息定义即可覆盖业务已有 topic；
- ROS2 与 IBMW 使用相同事件边界、统计口径和有效性规则；
- 默认关闭时不影响正常业务性能；
- 开启后有明确、可验收的扰动预算；
- 用户通过统一 CLI 完成能力探测、采集、验证和报告；
- 无法证明逐消息关联时明确降级，不输出看似精确但因果关系错误的时延。

因此，通用时延不能继续以“业务生成 artifact”为主路径，也不能只增加一个通用 subscriber。推荐建设**类型无关 Trace + Middleware 关联适配 + 统一证据控制面**的 Hybrid 方案。

## 2. 继承的设计约束

本方案必须服从现有设计，而不是另建一套口径：

1. **公平对标**：ROS2 与 IBMW 只有事件边界、场景合同、资源范围、采样策略和时钟质量一致时才可比较。
2. **低侵入**：不修改业务逻辑、消息 IDL/定义或 payload；不得通过增加 observer 默默改变 DDS matching、ACK、history 或背压。
3. **性能优先**：编译关闭路径没有新增热路径代码；运行关闭路径不读时钟、不分配、不加锁、不写文件。
4. **显式启用**：沿用默认关闭原则；支持代码初期由专用构建选项启用，运行时仍默认为 `off`。
5. **不破坏 ABI**：不修改已冻结的 Transport Driver vtable 和 capability V1 布局；新增能力使用内部 hook 或版本化 sidecar。
6. **零拷贝不退化**：不得为采集给 POD/loan payload 增加隐藏 header、复制、序列化或 fallback。
7. **证据 fail-closed**：业务运行不因观测失败而失败，但 required 时延证据在事件丢失、关联不唯一或时钟不合格时必须失效。
8. **当前平台边界**：第一阶段仍为 Linux 单机跨进程；跨主机单向时延不在首版承诺内。

## 3. 指标、交付键与事件边界

### 3.1 正式主指标

统一主指标定义为：

```text
publish_to_callback_ns = callback_enter_mono_ns - publish_enter_mono_ns
```

该式只在同一 `DeliveryKey`、同一 clock domain 且 `event_boundary_id` 完全相同时成立。指标包含发布入口之后的 filter/type-erasure、序列化、传输、take/receive、executor 排队和目标回调分发，但不包含用户 callback 自身执行时间。不同 boundary ID 的结果只能各自报告，禁止计算时延比值、差值或优劣结论。

“发布入口”和“目标回调入口”不能停留在概念函数。P0a 必须对实际构建列出并验证唯一函数级 hook：

| 系统 | P0a 候选 hook | 时间点 | 必须证明的包含关系 |
|---|---|---|---|
| ROS2 | 实际调用的 `rclcpp::Publisher*::publish` 重载或其唯一公共下层入口 | enter/return | ordinary/plain/serialized/loaned、intra/inter 分别说明是否经过；发布端 type-erasure 和 RMW 调用位于哪一侧 |
| ROS2 | 实际 executor dispatch 到用户 callback 的唯一入口 | enter/return | take、subscription filter、type-erasure、executor queue 位于哪一侧；hook 必须紧邻目标用户 callback，而非 middleware callback |
| IBMW | `ibmw_transport_sender_base_t::send` 对应实现或 `TransportDriverDispatcher::Send` 的唯一选定入口 | enter/return | framework async publish filter、`MessageFrame` 构造和逐 Driver fan-out 位于哪一侧 |
| IBMW | ordinary `MessageHandlerWrapper::callback` 最终 invoker，以及 loaned/retained 各自最终 callback invoker | enter/return | 当前 Dispatcher callback wrapper 只包到 IBMW function invoker；若 API 层在 `context.h` Dispatch 后才由异步 executor 进入用户 callback，则该 hook 不是最终 callback；P0a 首版必须排除异步 filter/executor |

表中名称是 P0a 待核实候选，不是已冻结 ABI。退出条件是：每条已验收路径只有一个入口/返回事件，递归与异常/失败返回不重复计数，并用调用图、最小 trace 和受控延迟注入证明 ROS2 与 IBMW 的包含关系等价；不能证明则使用不同 `event_boundary_id`，不得进入 P0b 的可比较合同。

### 3.2 SampleKey 与 DeliveryKey

一次逻辑发布的键为：

```text
SampleKey = run_epoch + publisher_entity_id + uint64_sequence
            + topic_id + type_id
```

每一个实际目标交付的校验键为：

```text
DeliveryKey = SampleKey + transport_path_id + subscriber_endpoint_id
              + callback_registration_id
```

- `run_epoch` 覆盖进程启动/重启边界，防止序号复用；
- `publisher_entity_id` 区分同 topic 多 publisher，sequence 是该 publisher endpoint 局部单调 64 位值；
- `transport_path_id` 区分 multi-driver、intra/inter 和其他并行传输路径；
- `subscriber_endpoint_id` 区分 fan-out 的每个目标 endpoint；
- `callback_registration_id` 区分同 endpoint 上独立注册、注销后重注册和 retained/ordinary callback；
- DDS 优先使用经 P0a 证明的 writer GUID + publication sequence 作为 native SampleKey 来源，不修改 payload；非 DDS 路径使用框架 endpoint epoch + sequence；
- 进程局部指针不可作为跨进程身份。禁止用 topic、时间最近邻、payload hash 或指针把 `stage_only` 包装为精确关联。

发布正确性先按 SampleKey 核对预期 transport path 和目标集合，再按 DeliveryKey 对每目标 delivery 单独校验。对支持 fan-out 的 ordinary 路径，一个 publish 可以合法对应多个 DeliveryKey；不再使用“publish/callback 一一关联”的假设。当前 loaned 路径例外：其语义是 first-success-wins，`source_driver` 只标识一个成功路径，不支持 multi-driver fan-out，必须按单 path capability 单独验收。

### 3.3 分段指标与支持等级

| 指标 | 定义 | 用途 |
|---|---|---|
| `publish_service_ns` | `publish_return - publish_enter` | 发布调用本地成本 |
| `middleware_delivery_ns` | `take_or_receive - write_commit` | Transport/middleware 交付 |
| `dispatch_queue_ns` | `callback_enter - take_or_receive` | Executor/调度排队 |
| `callback_duration_ns` | `callback_exit - callback_enter` | 业务回调耗时，仅诊断 |

`write_commit` 定义为 Driver 在本地 transport API 返回成功/接受后的事件，不是远端 reader 到达确认。DDS 对应 `DataWriter::write` 成功返回；Iceoryx 对应 publish 调用完成，但当前 API 可能是 `void`，无法证明本地接受时必须记为 `write_commit_unconfirmed` 并降为 `stage_only`。远端 delivery 由 `take_or_receive` 与 `DeliveryKey` 独立证明。不同 RMW/Driver 的边界仍须分别登记，分段指标同样只有 boundary ID 相同时才可比较。

| 等级 | 能力 | 可否正式比较 |
|---|---|---|
| `exact_correlated` | 对已采样、确认成功且观测事件完整的 DeliveryKey，发布与目标 callback 的因果链和时钟精确 | 仅在 run 有效性门禁通过后可以 |
| `stage_only` | 有阶段分布，但不能可靠连接到每目标 callback | 仅阶段指标 |
| `observer_latency` | 测量额外 observer，不代表目标业务 callback | 只能单列 |
| `unsupported` | 无可靠边界、身份或时钟 | 不可以 |

`exact_correlated` 描述的是纳入统计样本的因果精确性，不表示每个成功 DeliveryKey 都被采样或形成完整事件链，也不单独保证 run 有效。run 有效性另由预注册采样覆盖合同、trace loss 合同、delivery correctness、拓扑、时钟和 finalization 等证据门禁共同决定。

## 4. 候选方案与已知限制

| 方案 | 通用性 | 精确性 | 对正常性能影响 | 主要问题 |
|---|---|---|---|---|
| 业务埋点/消息时间戳 | 低 | 受控场景高 | 可控 | 需要改业务或消息，当前方案即此边界 |
| Generic Observer | ROS2 类型层面较高 | 只精确到 observer | 新增 endpoint 和流量 | 改变 DDS matching、可靠传输、SHM/网络负载及路径选择 |
| Stock `ros2_tracing` | 路径相关 | ordinary typed inter-process 阶段分析可用 | 开启时较低到中 | callback tracepoint 不带 sample identity，其他路径不自动继承能力 |
| Fast DDS Statistics | DDS 内部 | entity/endpoint/locator 等粒度的统计 | 中 | Humble 官方构建默认未启用，不能逐样本连接 ROS 目标 callback |
| Middleware/Driver 插桩 | 按适配器扩展 | 对已证明路径高 | 可分档 | patched RMW/Fast DDS 的部署、版本和长期维护成本 |
| Hybrid | 逐能力扩展 | 可证明时高 | 可分档 | 工程量、ABI 和版本矩阵维护成本最高 |

### 4.1 Generic Observer

Observer 只用于诊断和交叉检查：

- 新 subscriber 会改变 discovery、matching 和 reliable writer 资源行为；
- `GenericSubscription` 可能使原本 intra-process-only 的 publisher 新增 inter-process 路径，从而改变被测系统；
- Humble GenericSubscription 不覆盖纯 intra-process，也不能代表原目标 subscriber 的 executor 排队；
- stock generic callback 不暴露 `MessageInfo`；底层 RMW 可能提供可选 publisher GID/sequence，但必须逐 RMW 探测，不能据此承诺通用关联；
- 多 publisher、多目标和 IBMW 非 DDS Transport 不能由一个 observer 统一代表。

保留时必须标记 `measurement_kind=observer_latency`，并把 observer endpoint、CPU、内存和新增流量计入资源证据。

### 4.2 Stock ROS2 Tracing 与 DDS Statistics

Humble stock tracing 在本文严格限定为 **ordinary typed inter-process 的阶段观测候选**。`callback_start` 不携带 sample identity；发布、take 与 callback 的进程局部对象/缓冲区身份不能自动组成跨进程 DeliveryKey。下列组合必须是彼此独立的 capability，不得由 stock ordinary 路径外推：

- ordinary typed inter-process；
- serialized publish；
- true loaned publish/receive；
- pure intra-process；
- mixed intra/inter-process，同一 publish 的两条 delivery path 分开登记。

Fast DDS Statistics 的统计项具有 entity、endpoint pair、locator/transport 等不同粒度，不能统一描述为 endpoint pair 聚合；ROS2 Humble 官方构建默认未启用 Statistics Module，即使启用也不能把每个 DDS sample 连接到 ROS 目标 callback。它只能提供独立诊断证据，不能替代 correlation adapter。

## 5. 推荐 Hybrid 架构

### 5.1 分层与事件模型

```text
mw-perf-collect Controller / Evidence Plane
    |
    +-- ROS2 Trace Adapter
    |     +-- ros2_tracing/LTTng stage events
    |     `-- version-locked RMW/DDS correlation adapter
    +-- IBMW Trace Adapter
    |     +-- common publish/callback hooks
    |     `-- candidate versioned Driver correlation sidecar
    +-- Normalizer / Correlator
    |     +-- SampleKey and per-target DeliveryKey joins
    |     +-- correctness, clock and trace-loss gates
    |     `-- per-capability statistics
    `-- manifest / validation / report / compare
```

控制面继续由 `mw-perf-collect` 管理 managed 生命周期、环境、进程树、资源、provenance 和报告。Normalizer 不在业务热路径运行。本提案建议 `mw_perf.comm_trace.v1`，事件至少包括 `publish_enter`、`publish_return`、`write_commit`、`take_or_receive`、`callback_enter`、`callback_exit`、`transport_drop_or_error`、`trace_drop`、`topology_generation` 和 `topology_change`。`topology_change` 必须覆盖 endpoint/reader/Driver 的 register/unregister、transport path attach/detach、callback register/unregister/reregister 及其他会改变冻结 manifest 的动作，并记录变化前后 generation 和对象 ID。

公共字段至少为：

```text
schema, run_id, run_epoch, host_id, process_id, process_start_ticks,
thread_id, clock_id, time_namespace_id, mono_ns, event,
topic_id, type_id, publisher_entity_id, uint64_sequence,
transport_path_id, subscription_id, reader_id,
subscriber_endpoint_id, callback_registration_id,
native_writer_id, native_sample_identity, native_reader_identity,
transport, adapter, capability_id, event_boundary_id, sampled,
topology_generation, topology_change_kind, topology_phase
```

其中 `subscription_id`、`reader_id` 和 `callback_registration_id` 必须随接收链传到最终 callback dispatch；不能只在 take 点记录。每个数据事件必须关联其 `topology_generation`；控制面从 capability freeze 起持续监测到 artifact finalization，并区分 `measurement` 与 `controlled_teardown` phase。原始热路径事件使用注册后的固定宽度 ID，不写 topic/type 字符串，不执行 JSON、字符串复制或动态分配。

### 5.2 ROS2/Fast DDS 可行性边界

在 P0a feasibility spike 通过前，**不承诺 Fast DDS/RMW exact**。P0a-core 只验证首个推荐组合；ordinary typed、Plain type、serialized 和 true loaned 等其余路径属于 P0a characterization/独立 capability，不是 P0a-core 通过条件：

1. write 前后何处能取得唯一 native writer/sample identity，write 失败时是否产生/消耗 sequence；
2. `SampleInfo`、read/take 或等价 API 实际暴露的 writer GUID、publication sequence 和 reader identity；
3. native identity 从 receive/take 穿过 type-erasure、subscription、executor queue 到目标 callback dispatch 的完整传递链；只传到 take 不合格；
4. `WriteParams` 或当前锁定版本的等价 API 是否能无 payload 修改地取得/设置所需 identity，且不会改变 DDS 语义；
5. fan-out、多 reader、mixed intra/inter、重试、duplicate、reliable retransmit 和进程重启下 DeliveryKey 是否唯一；
6. patched `rmw_fastrtps_cpp`/Fast DDS 的符号、构建 ID、部署镜像和版本矩阵能否被 `doctor` 准确识别。

任何一项失败，该路径降为 `stage_only` 或 `unsupported`，不得输出 exact E2E。首个精确组合只承诺“**spike 通过并完成 P2 验收的路径和类型类别**”；不承诺任意消息类型，也不预先承诺 Humble + Fast DDS 必然可行。characterization 未完成不阻塞首个 ordinary exact，但必须按独立 capability 记录，不能由首个组合外推。Cyclone DDS 及每个未验收组合继续独立登记。

#### 当前 DDS 插件 identity trace 能力

启用 `IBMW_ENABLE_COMM_TRACE` 后，DDS 插件以 writer GUID 和 publication sequence 构成精确 `SampleKey`，并以 `SampleKey + reader GUID` 构成精确到 DDS reader delivery 的 `DeliveryKey`。同步 callback registration ID 已从 dispatcher 注册期传到最终 callback invoker，并由独立 exact stream 记录；旧 V1 dispatch 事件仍保持 `stage_only` 语义。该链在三套 DDS 运行时完成 selected capability 的 formal P0a completion，状态为 `valid_for_p0a_completion=true`；异步 executor dispatch 当前明确计为 unsupported，不生成 exact callback span。`metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`，不得作性能接受、排名或 latency advantage 声明。

### 5.2.1 现有 ROS2 环境的无业务修改落地与版本合同

#### “无感”边界与部署状态

本文的“无感”只表示业务契约不变：业务源码、业务二进制、IDL/消息定义和 payload 均不修改，不新增 generic subscriber，也不要求业务重新发布或写入时间戳。它**不等于运行环境不变**：RMW/Fast DDS、collector、trace session、动态加载库、环境变量、CPU/内存/磁盘和调度开销都必须被发现、记录并纳入报告。每次 run 必须明确标记：

| 字段/状态 | 含义 |
|---|---|
| `production_untouched` | 业务制品与正式运行库均未替换；只允许声明已验证的外部阶段观测或无侵入诊断，不自动获得 exact |
| `validation_instrumented` | 业务源码/二进制/IDL/payload 不变，但使用隔离 prefix/镜像及版本锁定的 patched RMW/Fast DDS 或观测构建进行验证 |
| `business_modified` | 修改业务源码、二进制、IDL 或 payload；属于另一类受控 benchmark，不得伪装为无业务修改 |

报告至少包含 `business_unchanged`、`runtime_changed`、`measurement_kind` 和 `deployment_mode`。其中 `business_unchanged=true` 不能覆盖 `runtime_changed=true`；`exact_correlated` 还必须有实际加载库、身份穿透和 finalization 证据。

#### 实际部署机制矩阵

| 机制 | 业务是否修改 | 适用定位 | exact 结论与限制 |
|---|---|---|---|
| 原生 `ros2_tracing`/LTTng | 否 | 外部阶段观测、tracepoint 验证 | 默认 `stage_only`；不自动携带 sample identity 到目标 callback |
| RMW C ABI `LD_PRELOAD` | 否 | 低风险 rcl/rmw C ABI 实验和诊断 | 不作为正式 exact；C ABI 边界可用，但 Fast DDS C++ private/inline/hidden/template 符号不稳定 |
| 隔离 validation overlay/兼容替换的 patched `rmw_fastrtps`/Fast DDS | 否 | 正式 exact 候选 | 正式 exact 首选；必须锁定版本、prefix/镜像、补丁、构建产物和实际解析结果 |
| Fast DDS public API 旁路 reader | 否 | 隔离 domain/独立 harness 中的 DDS 层诊断和交叉检查 | 在真实被测 domain 创建 reader 必然新增 DDS endpoint，会影响 discovery/matching、ACK/NACK、history/resource 和实际路径；必须标记 `runtime_changed`、`measurement_kind=observer_latency`，不得进入正式 measurement；只能在隔离 domain/独立 harness 使用，不能声称“不改变 matching”或直接 exact |
| eBPF/uprobes | 否 | 外部观测、稳定函数诊断 | 默认 `stage_only`；只在 P0a 验证稳定函数和身份语义后使用 |

若运行时完全不能替换或隔离加载目标 RMW/Fast DDS，不能声称 `exact_correlated`，只能报告已有 trace/外部观测等级。`LD_PRELOAD` 的 C ABI 仅覆盖稳定的 C 边界；Fast DDS 的 C++ private、inline、hidden、template 实现受优化、链接和版本影响，不能作为正式跨版本合同。正式路径必须由 collector 在启动时加载隔离 overlay/观测镜像，且不覆盖或污染 `/opt/ros`。

#### ROS2 实际打桩链与身份要求

目标链路是：

```text
rclcpp/rcl -> rmw_fastrtps_cpp/dynamic_cpp -> Fast DDS
    -> rmw receive/take -> subscription/type-erasure
    -> executor queue/dispatch -> target user callback
```

业务源码无需修改，但启动时由 collector 负责加载 validation overlay 或受控 preload，并记录解析到的库。Fast DDS 单独打桩不够：native writer/sample/reader identity 必须从 Fast DDS 经过 RMW、接收与 type-erasure，继续穿过 subscription 和 executor，直到最终 callback dispatch；只在 `write`、reader 或 `take` 命中不构成 E2E 证明。不得增加 generic subscriber 以“补齐”身份或 callback。

`patched Fast DDS/RMW` 不是完整方案。每个 exact candidate 必须冻结实际 patch/overlay surface，至少逐项覆盖 Fast DDS 的 write/read/take identity 获取、`rmw_fastrtps` 的 receive/take，以及 `rcl`/`rclcpp` 的 subscription/callback registration 和 executor dispatch metadata 传递，并证明同一 identity/metadata 到达最终 callback。业务二进制不改不等于所有依赖库都可替换；若 `rcl`/`rclcpp` 共享库、tracepoint 或 callback wrapper 无法可靠覆盖，或遇到静态链接、inline/hidden callback wrapper、ABI 不匹配，该候选必须 `stage_only`；若 required exact 所需 surface 无法验证则 `required_stop/invalid`，不能用部分 hook 命中补足。

#### API/ABI 版本合同

版本合同必须分层登记，不得以 ROS distro 名称或 API 名称推断 ABI：

| 层 | 必须独立评估的表面 |
|---|---|
| ROS API | `rclcpp` 公共 C++ API；`rcl`/`rmw` 公共 C API |
| RMW 实现 | `rmw_fastrtps_cpp` 内部符号、动态类型实现和补丁集 |
| DDS/CDR | Fast DDS public/private C++ API、Fast CDR API/ABI |
| 系统运行时 | compiler、`libstdc++`、glibc、链接器和编译选项 |
| 观测工具 | LTTng/UST ABI、tracepoint 定义、uprobes 目标 symbol 和加载方式 |

`doctor` 和每次 run 必须保留实际 `/proc/<pid>/maps`、库 SONAME、build-id、文件 SHA256、RMW/DDS patchset、编译器与关键编译选项、动态链接 resolution、内核/LTTng/uprobes 信息。当前仓库提出的 `Humble + rmw_fastrtps_cpp + Fast DDS 2.6.12 + Fast CDR 1.0.29` 只是首个待验证候选组合/构建候选，不是已锁定或已验证的运行期单元；仓库现有 ROS2 兼容记录还包含 Fast DDS 2.6.11（见 `src/tools/ibmw_gen/ros2_interface_import.py`），必须如实纳入矩阵。仓库版本是 SDK build lane，不等于 ROS2 运行期实际加载版本。只有 P0a 的 maps/SONAME/build-id/SHA/resolution 和 patch surface 证据全部通过后，具体组合才可登记 `verified_exact` 或 `verified_stage_only`，此前保持 `not_tested`；结论不外推到 Jazzy、Fast DDS 3.x 或其它 RMW。

#### Hook 合同与路径矩阵

每个 hook 必须记录 `symbol/tracepoint`、`enter/return`、递归处理、异常/失败返回、跨线程语义、identity 来源及是否到达 callback，并按 `typed`、`serialized`、`loaned`、`intra-process`、`inter-process` 分别登记。最低路径矩阵如下：

| 路径 | 必须验证的边界与身份 | 结论要求 |
|---|---|---|
| typed/inter-process | publish/write、RMW receive/take、executor callback | native identity 到 callback；否则 `stage_only` |
| serialized/inter-process | serialized publish/take 与目标 callback | 证明序列化边界不改变关联 |
| loaned | loan/write、receive/release、callback | 证明 loan ownership、失败终态和单 path 语义 |
| intra-process | rcl/rclcpp intra path 与 callback | 不得用 inter-process hook 外推 |
| mixed intra/inter | 两条实际 delivery path | 分别建立 `transport_path_id` 和能力结论 |

hook 命中只证明某个函数或 tracepoint 被执行，不等于通信链路、目标 callback 或 DeliveryKey 已被证明；调用图、最小 trace、实际 identity 和受控故障注入必须共同成立。

#### P0a 真实验证夹具

P0a 必须在目标机上保存版本快照和实际 maps/resolution，并完成：

1. 检查 `ros2_tracing` tracepoint 是否存在、字段是否符合当前构建，运行最小 typed demo pub/sub；
2. 用 C ABI `LD_PRELOAD` 仅验证低风险 `rcl`/`rmw` 边界，不把 C++ Fast DDS hook 作为正式依据；
3. 验证隔离 overlay 确实被加载且只替换预注册库，记录原始/观测镜像 resolution；
4. 仅在隔离 domain 或独立 harness 中用 Fast DDS public API 旁路 reader 做诊断交叉检查；真实被测 domain 的旁路 reader 必然新增 DDS endpoint，可能改变 discovery/matching、ACK/NACK、history/resource 和路径，必须标记 `runtime_changed`、`measurement_kind=observer_latency`，不得进入正式 measurement，也不得声称“不改变 matching”；
5. 仅对稳定、可重复解析的函数验证 uprobe 命中，不能命中则降级；
6. 建立无观测基线、观测开关基线，以及延迟、丢包、重复、乱序和 trace loss 故障注入；
7. 将真实业务 shadow run 与独立 harness 分开：harness 验证 hook/身份/注入，真实业务只验证业务 topic、QoS、payload 和启动合同未变化。

#### 真实性、降级与停止状态机

状态转移必须遵循：`preflight -> resolution_verified -> overlay_loaded -> topology_frozen -> measurement -> seal/finalize`。进入 `measurement` 前必须证明实际库已加载、业务 topic/QoS/payload/启动命令未变，并确认真实被测 domain 无旁路 observer endpoint；旁路 reader 只能属于隔离 domain/独立 harness 诊断。还要完成注入延迟/丢包的可观测性校验。版本、符号、identity、patch surface 或 overlay 真实性不合格时，required run 直接 `required_stop/invalid`，不以部分数据补足。

identity 到不了最终 callback 时，结果只能是 `stage_only`。trace/session/collector 失败时业务继续运行，但 required 证据必须 fail-closed，报告记录 `evidence_failed` 和原因。业务继续运行不代表测量成功，也不允许把旁路 reader、hook 命中或阶段分布包装成 exact。

#### 部署、资源与回滚

validation 使用独立 prefix、容器/观测镜像和独立 artifact 目录；启动前完成动态链接 resolution 和环境快照，不覆盖、不污染 `/opt/ros`。`LD_PRELOAD` 必须由 collector 显式设置、在启动命令和报告中记录，并在 teardown/回滚时显式清除。overlay、collector、LTTng session、ring buffer、trace 文件、CPU、内存和磁盘均计入资源预算。任一加载、兼容性或观测失败都必须停止 required 采样，关闭 collector 后回到原始运行库和原始启动环境，并保留失败证据与回滚记录。

### 5.3 IBMW 现状与目标差距

IBMW ordinary `Send` 有两个入口：`Send(SendAccessInfoWrapper&&)` 和 `Send(MessageFrame&&)`。当前 P0a-core candidate 已在第一条入口实现注册期缓存的观测 overlay，并在 Dispatcher 接受边界捕获 fixed-width publish envelope；第二条入口仍只具备 stage 语义。既有 `RegisterSendType` topic 级 `uint32_t` sequence、Context `ibmw-pub_seq`、`TransportInfo.index` 和 `system_clock` 语义 `ibmw-pub_ts` 仍不是正式 SampleKey 或 monotonic 主指标时间戳。candidate 的跨 DDS identity 使用 writer GUID + native sequence，并以 reader GUID 派生 DeliveryKey；该结论只覆盖 DDS ordinary non-plain、单 Driver、同步 callback，不外推到其它入口、Driver 或异步路径。

P0a 必须定义并验证：

- publisher endpoint 注册对象、subscriber endpoint 注册对象和 callback registration 对象的创建、注销与不可复用 ID；
- 每 publisher endpoint 的 `uint64_t` counter，以及随机/持久化 run epoch、进程重启和 counter wrap 合同；
- ordinary `Send`、DDS ordinary non-plain、`plain_loan_write`、`plain_fallback_write`、serialized/loan，以及 iceoryx ordinary/serialized-loan/retained/POD 每条路径的 identity 来源；
- 对支持 fan-out 的 ordinary 路径，multi-driver 时一个 SampleKey 如何派生每个 transport path 和目标 endpoint 的 DeliveryKey；当前 loaned 路径按 first-success-wins 和单一 `source_driver` path 建模，不纳入 multi-driver fan-out，除非未来另行定义并冻结 multi-loan 合同；
- receive identity 如何穿过 `MessageFrame`/Context 或等价 side metadata 到最终 callback registration；当前 `MessageFrame` 无 token 字段，且 `MessageHandlerTool` 会重建 frame，Context string/map 也不是固定宽度 token，不能把现状描述为已可行；
- write/publish 失败、filter drop、异步 filter 切线程和 shutdown drain 时 identity 的终态。

### 5.4 ABI inventory 与 sidecar 候选

P0a 必须形成逐字段 ABI inventory，不能只写“不修改 Driver vtable”：

| 现有表面 | 约束/风险 |
|---|---|
| `TransportDriver` 虚表与 `TransportDriverCapabilitiesV1` bundle | 已冻结；`Send` 返回 `void`，V1 capability 指针布局不能追加逐样本合同 |
| `MessageFrame`、`SendTypeWrapper` | 跨 core/driver 的 C++ 布局；含缓存指针、Context 和容器，直接加字段有 ABI 风险 |
| `ibmw_transport_sender_base_t`、`ibmw_transport_receiver_base_t` | C sender/receiver vtable 无 `abi_version/struct_size`，注释已限定新旧 provider 兼容性 |
| `ibmw_transport_context_base_ops_t` | Context ops 无版本/大小字段，KV ownership、分配和线程语义需盘点 |
| `LoanedSampleHandle`/`ibmw_loaned_sample_handle_t` | C/C++ 镜像布局并含进程局部 opaque pointer，不能直接承担跨进程 identity |
| `ibmw_reporter_v1` | 适合累计 slot；不是逐消息事件流，生命周期和调用面不足以承载 callback correlation |

独立、版本化 C sidecar 仅是候选，不在 feasibility 前冻结。若采用，P0a/P0b 必须定义 query symbol、`abi_version`、`struct_size`、reserved slots、可选函数探测、字符串/事件/handle ownership、并发与 callback thread 规则、异常边界、flush/finalization、shutdown/quiescence 顺序和 provider 消失行为。只有 spike 证明可部署、性能可接受且所有路径能传递 DeliveryKey 后，P0b 才冻结 sidecar ABI；否则选择内部 hook 或将能力降级。

### 5.5 iceoryx 与 DDS 路径矩阵

| capability | identity/语义要求 | P0a/P3 出口 |
|---|---|---|
| iceoryx ordinary | Context/header 不改变 payload，覆盖每 endpoint callback；当前 publish 接受状态和 metadata 穿透未证 | 先 `stage_only`，独立验收 |
| iceoryx serialized loan | loan/publish/receive/release identity 和失败终态 | 独立验收 |
| iceoryx retained receive | callback registration、retained ownership和延迟 release 不混淆 delivery | 独立验收 |
| iceoryx POD-only | 不得增加 header、复制或 fallback | 在证明 `(segment, offset, generation)` 跨进程唯一且防 chunk 复用前为 `unsupported` |
| DDS `plain_loan_write` | Plain loan write 的 loan、write commit 和 identity 穿透 | 按独立 capability 验收 |
| DDS `plain_fallback_write` | Plain fallback write 的复制/fallback、write commit 和 identity 穿透 | 按独立 capability 验收 |

Core Metrics 继续保留累计计数和生命周期诊断，与逐消息 Trace 独立。`driver_send.returned` 不是 `write_commit`，callback 累计数也不能反推时延。

### 5.6 IBMW实现拓扑与延迟合同

以下拓扑对应当前唯一选定的窄能力 `dds.ordinary.nonplain.single_driver.sync_callback`。其 clock/topology/trace-quality 事实已绑定进 validated source/formal chain，并完成 formal P0a completion；v8 exact source 仍为不可变 `candidate_only=true`/`P0A_ARTIFACT`，v9 formal snapshot 不回写 v8。top-level 仍保持 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`。

#### 推荐拓扑与非侵入边界

```text
mw-perf-collect control/evidence plane
    -> IBMW comm trace runtime
       (internal hooks + per-thread/per-CPU bounded ring)
    -> normalizer/correlator sidecar
```

内部 hook 负责事件和 metadata 穿透；sidecar 只消费、关联、验证和报告，不进入逐消息关联热路径。不得扩展 frozen Driver vtable 或 C API。业务源码、业务二进制、IDL 和 payload 均不修改；trace runtime 在 IBMW 内部通过构建开关提供，验证使用 overlay 或独立镜像。`compile-out`、`runtime-off`、`sampled`、`full` 的门限继承第6节；`off` 时不得读时钟、分配、加锁或写文件，不新增 observer。

DDS 观测构建提供 active-only v2 binary exporter。managed sink 通过控制面传入的安全目录 FD 写出绑定 identity/topology 的 canonical meta、固定 256-byte little-endian segment header、64-byte stage event payload 和最终 seal；后台唯一 consumer 在运行期持续 drain，shutdown 只由最后一个 extension owner 完成 seal。V1 query ABI 保持不变，新增 V2 query 发布采样与导出计数。stage stream 仍只有 `publish_write`、`take_receive`、`dispatch`；另有独立 sealed 72-byte exact callback `.ibce` stream。validator 必须绑定两类 stream identity 并做因果关联，不能把三个 stage record 本身称为 exact callback。

#### 当前 ordinary Send 与 callback 边界

普通发送必须按当前源码验证两条入口：

```text
Send(SendAccessInfoWrapper&&)                 [当前约655附近]
  -> catalog 四元组 lookup -> context type/used
  -> pub_seq/pub_ts -> cached filter -> cached Driver fan-out
  -> Driver::Send(MessageFrame&)

Send(MessageFrame&&)                          [当前约913附近]
  -> shared_ptr<MessageFrame> -> context
  -> topic sequence/filter/driver map lookup -> fan-out
```

每个入口事件必须记录 `overload_id`。`publish_enter` 位于 dispatcher 入口；`publish_return` 只表示本地调用返回，不表示 transport 已提交。每个 Driver 必须有独立的 `transport_path_id` 和 `write_commit`。`write_commit` 只记录本地 transport API 返回成功/接受：DDS 为 `DataWriter::write` 成功；Iceoryx 当前 publish API 若为 `void` 且无法证明本地接受，必须记为 `write_commit_unconfirmed`/`stage_only`。Core Metrics 的 `driver_send.returned` 不能替代 `write_commit`，远端 delivery 必须由 `take/receive` 和 `DeliveryKey` 独立证明。

普通接收和最终 callback 必须按以下真实边界验证：

```text
Driver OnMessage/on_data_available
  -> MessageHandlerTool::DoMessageCallback
  -> MessageHandlerWrapper wrapper
  -> subscribe filter -> CheckMsg/deserialize/cache
  -> final C callback invoker
     (transport_driver_dispatcher_ops.cc callback wrapper)
```

当前受控同步路径通过 40-byte fixed-width callback invocation envelope 将 enter/exit hook 显式传到最终业务 callback，并验证顺序为 `filter -> callback_enter -> business callback -> callback_exit`。该结论不覆盖 API 层异步 executor；若 `context.h` Dispatch 后才进入用户 callback，当前 wrapper 仍不能称为最终 callback。首个 P0a-core 排除异步 filter、executor、多 handler 和多 Driver；这些路径须纳入独立 patch surface 和 capability，未证明时只能 `stage_only`。

#### Identity 合同与 ABI 约束

正式内部 identity 新增：publisher endpoint id + epoch + `uint64` counter、subscriber endpoint id + epoch、`callback_registration_id`、`transport_path_id` 和 `run_epoch`。现有 topic 级 `uint32` `pub_seq` 与 `system_clock` `pub_ts` 仅作兼容/诊断字段，不是正式 `SampleKey`。`TransportInfo.index`、Context 指针、`source_driver`、`driver_token` 和 message pointer 禁止作为跨进程 identity。

方案已选择 A，且只对受控窄能力完成实现；B 保留为被拒绝的 stage-only 备选说明：

- **A（已选）**：在非公开内部观测构建中使用 32-byte publish envelope 和 40-byte callback invocation envelope，不修改 `MessageFrame`、`LoanedSampleHandle`、frozen Driver vtable 或 public C vtable。采用 runtime overlay/version lock；当前已验证同步 stack lifetime、异常 exit 和 registration fail-closed，异步/跨线程能力仍不在 scope。
- **B**：保持现有布局，仅在预注册的单线程/同步路径使用 per-call/thread-local token。它不能覆盖异步或跨线程路径；若采用 B，该能力只能 `stage_only`，除非 identity 能由 driver native identity 与 callback registration 闭合。

不得直接修改 `MessageFrame`、`LoanedSampleHandle` 或 C vtable，不采用 TLS 承担 exact identity。A 的 token 生命周期、所有权、注销和 shutdown 行为必须进入 P0a evidence；primary Send 热路径使用注册期缓存 overlay，不做 RTTI/map lookup。

#### Path matrix 与准确边界

| 路径 | 支持等级与必须验证的事件/边界 |
|---|---|
| ordinary | ordinary capability；P0a-core 先限单 Driver，后续可验证多 Driver fan-out；每个 Driver/目标 callback 独立 `DeliveryKey`，按冻结拓扑核对预期集合。 |
| DDS ordinary non-plain | 首个推荐候选；`writer->write` 后验证 native identity、API 语义及其到最终 callback 的传递；单 Driver、同步 callback。 |
| DDS `plain_loan_write` | Plain loan write；独立验证 loan、`write_commit` 和 identity 到 callback 的传递，不能由 non-plain 外推。 |
| DDS `plain_fallback_write` | Plain fallback write（含 `loan_sample + memcpy + writer->write`）；独立验证 fallback 语义、`write_commit` 和 identity 到 callback 的传递。 |
| Iceoryx ordinary | 当前 API 的 publish 接受状态和 side metadata 传递尚未证明；先作 `stage_only`，后续独立 capability 验收。 |
| publisher loan | `Loan()` first-success-wins，跳过普通 filter、`pub_seq`、`pub_ts`；`PublishLoaned`/`ReleaseLoaned` 为 void，`source_driver` 仅进程内 opaque。首版单 path；只有跨进程 identity 和终态证明通过才可 `exact`，否则 `stage_only`/`unsupported`。 |
| receiver loan | `AttachLoaned` 绕过 `MessageFrame`、subscribe filter 和反序列化，直接进入 payload callback；独立 capability，ctx 可能为 `nullptr`，必须验证 release 生命周期。 |
| retained | callback、release、shutdown quiescence 为独立事件；callback latency 不包含 delayed release。 |
| Iceoryx POD-only | 不改 payload/header；只有 `(segment, offset, generation)` 跨进程唯一且防 ABA 才为 `exact`，否则 `unsupported`。 |
| non-DDS network drivers | 可做 ordinary 阶段指标，但不能外推 DDS identity 或 DDS exact 结论。 |

#### 延迟、loan 和终态合同

普通路径正式指标为：

```text
publish_to_callback = callback_enter - publish_enter
```

两事件必须属于同一 `DeliveryKey`、同一 clock domain 和同一 boundary。分段指标为 `publish_service=publish_return-publish_enter`、`middleware_delivery=take/receive-write_commit`、`dispatch_queue=callback_enter-take/receive`；`callback_duration=callback_exit-callback_enter` 仅作诊断。事件至少包括 `loan_enter`、`loan_success`、`loan_fail`、`publish_loaned_enter`、`write_commit`、`write_commit_unconfirmed`、`publish_fail` 和 `release`。outstanding loan 不是 transport loss；void `PublishLoaned` 只证明调用返回，不证明 write commit。retained release 不扩大主 latency。

#### 业务无感、性能和 fail-closed

业务 fail-open：观测失败不得使业务发送或 callback 失败。证据 fail-closed：identity 缺失或重复、topology 变化、ring drop、callback 重注册、Driver fan-out 与预期不符、release/shutdown 不完整、时钟或 manifest 失败时，required 结果必须为 invalid/停止，不得降级包装成 exact。P0a-core 推荐固定 DDS ordinary non-plain、单 Driver、同步 callback；Iceoryx ordinary 先作 `stage_only`。异步 callback、Plain 两种写法、multi-driver、loan/retained/POD 等均作为 characterization/独立 capability，不阻塞首个 ordinary exact，但未证明时不得外推。

## 6. 采样、性能与扰动控制

### 6.1 开关与采样候选

```text
IBMW_ENABLE_COMM_TRACE=OFF|ON       # 编译期，初期默认 OFF
IBMW_COMM_TRACE=off|sampled|full    # 运行期，默认 off
```

- compile-out：无 trace 分支、状态和依赖；
- runtime-off：只允许可预测早返回/静态键，不读时钟、不查表、不写 buffer；
- sampled：目标模式，但算法须先通过 P0a spike；
- full：短时诊断和受控 benchmark。

sampled 的可实现候选是：`publish_enter` 先写入有界暂存项；write 获得 native identity 后，按 SampleKey 决策并提交完整发送事件，未选中或 write 失败则丢弃暂存。接收端用已传到 callback dispatch 的同一 native identity 决策。P0a 必须覆盖同一 publisher 并发、递归 publish、异步 filter 切线程、write 失败/抛错、暂存超时和溢出、ordinary 路径一个 publish 多 Driver、loaned 路径 first-success-wins、乱序 commit 与 shutdown drain，并证明不会把时间戳配给错误 sample。

如果该算法在目标路径不可实现，首版只能使用按 publisher 预采样窗口，或短时 `full` 后离线抽样；两端必须由控制面冻结同一窗口。每个 required run 必须预注册采样母数、选择算法、目标采样率/窗口、允许偏差和最小有效样本数，并保留逐 SampleKey 的选择证据；未采样的成功 DeliveryKey 属于 `sampling exclusion`，不要求事件完整，也不得计入 correlation 分母。不得虚构发送端和接收端能在 write identity 产生前独立得到一致采样决定。

### 6.2 热路径与性能矩阵

- 注册阶段完成 topic/type、endpoint、callback、Driver 和 trace handle 注册；
- 热路径禁止 mutex、unordered_map lookup、heap allocation、JSON、日志和同步文件 I/O；
- 使用 per-thread/per-CPU 有界 ring buffer和固定宽度事件；buffer 满时不阻塞业务，但独立记录 trace loss；
- Trace 与 Core Metrics 是两个独立能力，不能用 metrics on 代表 trace on，也不能合并开销归因；
- 提供 `perf-observable` 构建 preset；只有 runtime-off 长期通过后才讨论默认编译开启。

性能实验至少覆盖 `Core Metrics off/on` 与 `trace compile-out/runtime-off/sampled/full` 的交叉矩阵；每个 trace 档分别比较 metrics off/on，避免把累计计数成本算入 trace。建议预注册门限仍为：

| 对比 | CPU/吞吐 | p50 | p99 | 额外 PSS | 其他 |
|---|---:|---:|---:|---:|---|
| runtime-off vs compile-out | 不超过 1% | 不超过 1% | 不超过 1% | 不超过 2 MiB | 无 trace 线程、文件和时钟读取 |
| sampled vs runtime-off | 不超过 3% | 不超过 3% | 不超过 5% | 不超过 8 MiB | required 默认 trace loss = 0；例外须预注册非零上限 |
| full vs runtime-off | 不超过 5% | 不超过 5% | 不超过 10% | 不超过 16 MiB | 只用于短时采集 |

门限以配对 block 和置信区间上界判断，且不得事后放宽。若 required 明确允许非零 trace loss，必须在运行前预注册上限，受影响 DeliveryKey 必须剔除并单列数量和原因，报告不得声称 `100% coverage`。矩阵还要覆盖小 payload 高频、大 payload、1/多 publisher、1/多 subscriber、sync/async executor、支持 fan-out 的 ordinary multi-driver、loaned 单 path 和各独立 capability。

## 7. 能力探测与冻结

统一 CLI 保持为 `doctor/run/validate/report`。配置中的 `topics:["*"]` 仅表示选择所有已注册候选 topic，不代表它们都支持 exact。

能力门分成两个时点：

1. **静态 preflight**：`doctor` 在启动 SUT 前检查 distro/RMW、adapter 和 tracepoint ABI、构建 ID、运行镜像、LTTng 权限/版本、磁盘预算及配置可满足性；静态 required 不满足立即停止并写 invalid evidence。
2. **capability freeze**：SUT 完成 endpoint、reader、callback 和 Driver 注册并进入 readiness 后，控制面读取实际 topology 和 attached 状态，冻结逐 topic/type/path/endpoint/callback capability manifest 及其 `topology_generation`；required 不满足必须在 warmup/sample 前停止，并保留 manifest、原因、日志和已生成 artifact。

“attached”不等于可证明 complete finalization。控制面必须从 freeze 到 finalization 持续采集 `topology_generation/topology_change`。有效拓扑区间从 freeze 开始，到所有已采样 delivery 已完成或已有终态、ring buffer 已 drain 后写入 `measurement_topology_seal` 为止；该 seal 前的 register/unregister、path attach/detach、callback reregister 或新 transport path 均使本 run invalid，不能静默降级。seal 后才允许进入 `controlled_teardown`，预期的 reader/callback/Driver 注销不会使 run invalid，但必须带 phase 记录并与预注册 shutdown 顺序、最终空闲注册表和完整 generation 序列对账；非预期 teardown 变化、拓扑监测事件丢失、generation 不连续或无法完成任一阶段对账时，required 仍 invalid。若 adapter/sidecar 无法证明 seal、flush、drop counter、受控注销和 artifact finalization 完整，required 必须拒绝。

报告按 capability、topic/type、publisher、transport path、subscriber endpoint 和 callback registration 列出成功 delivery 数、各类 loss/drop、重复/乱序、p50/p90/p99/p99.9/max、采样率、clock 和 boundary ID。

## 8. 时钟合同

首版单机跨进程要求 ROS2、IBMW 发送/接收双方及 LTTng 使用同一明确 `clock_id` 和 Linux time namespace。若 LTTng trace clock 表示不同，必须记录 trace clock UUID/频率/offset，以同源校准事件执行可审计转换，并给出残差上界；无法证明同域则 unsupported。`CLOCK_MONOTONIC` 与 `CLOCK_MONOTONIC_RAW` 不能直接混减，也不能仅靠启动时一次 offset 混用。wall/`system_clock` 只用于报告时间和既有兼容字段，不进入主指标。

跨主机时 `CLOCK_MONOTONIC` 只在各自主机内有意义，绝不能跨主机相减。后续方案必须选择并冻结：

- 使用 `ptp4l` 驱动 PHC、`phc2sys` 驱动系统时钟并采集 servo 状态；或
- 双方直接读取同一合同下的 PHC/`CLOCK_TAI`，明确硬件/软件 timestamp 边界。

合同还必须说明 TAI/UTC 转换与 leap-second 数据来源、holdover 进入/退出、clock step 检测和 invalid 区间、频率漂移、路径不对称估计。最终误差界需合成 PHC/servo offset、timestamp/trace clock 转换残差、调度读取误差和路径不对称上界；单向结果报告 `latency +/- uncertainty`，超阈值即 unsupported。仅有 NTP 且误差不可量化时不输出精确单向时延。

## 9. 正确性、丢失与证据门禁

canonical 绑定根为 `capability/p0a_evidence_manifest.json`。它由每个 P0a run 生成并由 validator 消费；其 `schema` 至少为 `mw_perf.p0a_evidence_manifest.v1`，并包含预注册的 `run_type`、`observer`、`measurement_kind`、`run_id`、`capability_id`、`path_id`、`deployment_mode` 及完整 entry 集合。manifest 顶层必须包含不可变的 `registration_contract_sha256`、`registration_contract_version` 和 `registration_contract_ref`。该 registration contract 必须绑定 `run_type`、`observer`、`measurement_kind`、全部预注册采集配置和完整 artifact universe。manifest 只能引用该 contract，不能自行修改配置、降级 status 或新增/删除 entry。每个 entry 必须包含 `relative_path`、`sha256`、`record_count`、`schema_version`、`finalization` 和 `status`；manifest 自身也必须引用同一组 `run_id`、`capability_id`、`path_id`。canonical manifest 的逻辑结构为：

```json
{
  "schema": "mw_perf.p0a_evidence_manifest.v1",
  "run_id": "<run>",
  "capability_id": "<capability>",
  "path_id": "<path>",
  "deployment_mode": "<mode>",
  "registration_contract_sha256": "<immutable-contract-sha256>",
  "registration_contract_version": "mw_perf.registration_contract.v1",
  "registration_contract_ref": "registration/contract.json",
  "run_type": "p0a_proof",
  "observer": false,
  "measurement_kind": "exact_correlated",
  "required": [
    {"relative_path":"capability/resolution.json","sha256":"<sha256>","record_count":1,"schema_version":"<v>","finalization":"<state>","status":"required"},
    {"relative_path":"capability/hook_callgraph.json","sha256":"<sha256>","record_count":1,"schema_version":"<v>","finalization":"<state>","status":"required"},
    {"relative_path":"normalized/identity_trace.jsonl","sha256":"<sha256>","record_count":1,"schema_version":"<v>","finalization":"<state>","status":"required"},
    {"relative_path":"validation/fault_injection.json","sha256":"<sha256>","record_count":1,"schema_version":"<v>","finalization":"<state>","status":"required"},
    {"relative_path":"validation/rollback.json","sha256":"<sha256>","record_count":1,"schema_version":"<v>","finalization":"<state>","status":"required"}
  ],
  "conditional": [
    {"relative_path":"capability/observer_isolation.json","sha256":null,"record_count":0,"schema_version":"<v>","finalization":"not_applicable","status":"not_applicable", "reason":"observer=false; no observer endpoint was created"}
  ]
}
```

`status` 的唯一枚举为：`required`（该 entry 必须存在且必须提供实际证据）、`optional`（仅当 registration contract 明确声明时允许；一旦登记仍须按实际值校验）和 `not_applicable`（条件确实不适用）。validator 必须先用 manifest 顶层的 `registration_contract_version` 查找合同并校验其 SHA-256，再按 `run_type` 和 contract 中的完整 universe 推导每个 entry 只能是 `required`、`optional` 或 `not_applicable`；manifest 不能把配置项降级，也不能新增或删除 entry。`required`/`optional` 的 `record_count` 必须是 validator 按对应格式规则从实际文件重算出的非负整数；真实空文件的 `0` 合法，但 `0` 不能作为示意或占位值。JSON 示例中的 `1` 是实际文件计数示例，不是模板占位。只有 `not_applicable` 固定允许 `sha256=null`、`record_count=0`、`finalization="not_applicable"`，且必须有非空 `reason`；任何 status 与这些字段的组合不一致均为 invalid。`required`/`conditional` 是兼容现有示例的分组，不改变上述 status 语义。

五项证明文件 `resolution.json`、`hook_callgraph.json`、`identity_trace.jsonl`、`fault_injection.json` 和 `rollback.json` 对 `p0a_proof` 与 `exact_run` 为 required；`stage_run` 不得因此伪装成 proof，`observer_diagnostic` 按 observer universe 单独闭包。只有实际使用 Fast DDS 旁路 reader 或 observer 时，`observer_isolation.json` 才是 `required`；未使用且所在 run universe 包含该 entry 时，必须写明 `status=not_applicable` 及原因。采集/结果 artifact 不能替代上述证明。

### 9.1 run 类型与 artifact 闭包

`run_type`、`observer`、`measurement_kind` 和完整 registration contract 必须在 manifest 生成前预注册，运行中和 finalization 后均不可改变。以下映射是 contract 必须表达且 validator 必须执行的完整规则；validator 必须拒绝漏项、增加未注册项、把 `required` 降为 `optional` 或 `not_applicable`，以及把 `optional`/`not_applicable` 伪装成 required 证据：

| `run_type` | 权威 artifact 要求 |
|---|---|
| `p0a_proof` | `resolution.json`、`hook_callgraph.json`、`identity_trace.jsonl`、`fault_injection.json`、`rollback.json` 为 `required`；`observer=true` 时 `observer_isolation.json` 也为 `required`，`observer=false` 时该 entry 必须为 `not_applicable`；采集/结果 artifact 按实际配置另行登记，不得替代五项证明。 |
| `stage_run` | registration contract 必须列出完整 universe：`comm_trace`、`comm_events`、`latency_samples`、`communication_latency`、`communication_delivery`。启用项为 `required`，明确禁用项为 `not_applicable` 并给出原因；五项均须登记，不得漏报、删除或降级。 |
| `exact_run` | 所有预注册采集/结果 artifact（包括 `comm_trace`、`comm_events`、`latency_samples`、`communication_latency`、`communication_delivery`）均为 `required`，并附 `resolution.json`、`hook_callgraph.json`、`identity_trace.jsonl`、`fault_injection.json`、`rollback.json` 五项证明；不得以阶段结果替代任一项。 |
| `observer_diagnostic` | `observer_isolation.json` 为 `required`，且 `measurement_kind` 必须为 `observer_latency`；其他采集/结果 artifact 依预注册配置登记，observer 未隔离或 measurement kind 不符即 invalid。 |

`observer` 只能取预注册的布尔值；使用任何 observer endpoint 时必须为 `true`，并按映射要求生成 isolation evidence。`measurement_kind` 也只能取预注册且与 run 类型、实际观测路径一致的值；例如 `observer_diagnostic` 不得登记为 `exact_correlated`。run 类型、observer 或 measurement kind 的缺失、错误组合或运行中修改，均为 `required_stop/invalid`。

artifact 清单按用途分开记录如下；实际是否 required 由 canonical manifest 按 run 类型决定：

```text
采集/结果 artifact（按 run 类型登记）
raw/comm_trace/
raw/comm_trace/directory_finalization.json
normalized/comm_events.jsonl
normalized/latency_samples.jsonl
capability/communication_latency.json
validation/communication_delivery.json

P0a 证明 artifact（p0a_proof/exact_run）
capability/resolution.json
capability/hook_callgraph.json
normalized/identity_trace.jsonl
formal_anchor
validation/fault_injection.json
validation/rollback.json

条件 P0a 证明 artifact（实际使用 Fast DDS 旁路/observer 时）
capability/observer_isolation.json
```

所有 evidence、采集和结果 artifact 必须在文件内容中引用与 manifest 完全相同的 `run_id`、`capability_id` 和 `path_id`。manifest 不直接对目录计算 hash：`raw/comm_trace/` 必须递归展开为实际文件 entries，每个实际文件独立登记 `sha256`、`record_count`、`schema_version` 和 `finalization`，并按 UTF-8 相对路径的字节序排序。目录完整性由 manifest 中完整的文件集合及保留的 `directory_finalization` entry 校验；该 entry 必须提交最终文件集合、排序和目录 finalization 状态的实际证据，而不是目录 hash。空目录、实际存在但未列出的文件、manifest 列出但不存在的文件、重复文件、路径排序不一致或目录 finalization 未完成均为 invalid。

记录数规则必须预注册并由 validator 执行：JSONL 按物理记录行计数（空文件为 0，非 JSONL 物理行仍按格式校验失败）；JSON 按其 schema 定义的记录数计数，不能笼统按字节或顶层对象字段计数；binary 默认 `record_count=0`，只有预注册的 binary record 解析规则允许非零。对 `required` 和 `optional`，manifest 中的 `record_count` 必须是 validator 从真实文件按上述规则重算出的非负整数；真实空文件为 0 合法，但不得用 0 作为占位。不得把所有 artifact 强制为非空；若 registration contract 预注册了最小记录数，实际计数低于该值则 invalid。`not_applicable` 才固定使用 `record_count=0` 和 `finalization=not_applicable`。validator 必须以 manifest 为根重新计算每个文件的 SHA-256、记录数和 finalization/status，并检查 `resolution`、`hook_callgraph`、`identity_trace`、`fault_injection`、`rollback` 之间的 run/path/部署模式和结论一致性。manifest 缺失、路径漂移、hash/record count/finalization 不一致、required entry 缺失、status 降级、目录集合不完整或 `not_applicable` 没有原因，均为 `required_stop/invalid`；不得以部分 artifact 或阶段性分布替代。

manifest/hash 用于检测 artifact 缺失、错配、陈旧以及非恶意采集完成后的变化，不抵抗拥有结果目录写权限的主动攻击者。若需抵抗主动篡改，必须使用目录外签名或远端证明；该能力不纳入 P0a。

`mw_perf.p0a_contract.v3` 首次将 `capability/resolution.json` 升级为 collector-owned `mw_perf.p0a.resolution.v2`。collector 在 `Popen` 前以 FD 打开并哈希每个预注册 canonical path，冻结 SHA-256、device、inode 和 size；managed exact run 中再通过 `/proc/<pid>/exe` 和 executable maps 的 FD identity/hash 轮询观测根 SUT。该 proof 只证明 prelaunch FD snapshot identity 与 managed run 期间 collector 至少一次轮询观测完全匹配，不证明镜像在所有采样时刻持续存在。其 threat model 为 `cooperative_contained_sut_no_active_same_uid`，不抵抗 active same-UID 对手。当前 collector-owned resolution artifact 已在三套 DDS 运行时通过，并纳入 selected capability 的 formal chain。

`mw_perf.p0a_contract.v4` 在 v3 基础上将 `normalized/identity_trace.jsonl` 升级为 collector-owned `mw_perf.p0a.identity_trace.v2`。它从封存的 stage/exact source set 重建每个 DeliveryKey 的完整因果边界，绑定 writer GUID + sequence、reader GUID、callback registration ID、stream/topology 和时间边界，并支持离线逐字节重算。该 proof 证明已成功 delivery 的 SampleKey/DeliveryKey 到 callback registration invocation 的因果链，不证明其它 capability。

`mw_perf.p0a_contract.v5` 在 v4 基础上将 `capability/hook_callgraph.json` 升级为 collector-owned `mw_perf.p0a.hook_callgraph.v2`。v5 在创建结果目录和启动 SUT 前要求 managed/full/`exact_run`、窄 DDS capability、`sut/exe` 与 `extension/maps` resolution declarations；collector 在 resolution 和 identity trace 成功后，以 exclusive create 发布 canonical semantic boundary chain。该 artifact 将可信固定 catalog 中的 `publish_write`、`take_receive`、`sync_dispatch`、`final_callback_invocation` 四个逻辑边界与已解析的 runtime image identity、raw stage/exact record counts、canonical DeliveryKey set 和时间顺序绑定，并可由离线 validator 从 archived raw evidence 逐字节重建。catalog 中的函数和源码路径是 trusted contract declarations，不是运行期函数执行观测；artifact 明确声明没有 dynamic stack、函数地址或 source-to-build provenance proof。当前 v9 formal snapshot 已将该 chain 与 formal anchor、fault/rollback companions 及声明闭包关联；v8 source 保持不可变 candidate-only/P0A_ARTIFACT。

`mw_perf.p0a_contract.v6` 在 v5 基础上将 `normalized/comm_events.jsonl` 与 `normalized/latency_samples.jsonl` 升级为 closed、collector-owned v2 results。collector 从已重建的 identity/resolution/hook inputs 生成 canonical projection：每个 SampleKey 只生成一条 publish span，每个 DeliveryKey 生成 receive、dispatch、callback 三条 span；每个 DeliveryKey 生成一条 `publish_start_to_callback_start` 纳秒样本，且 `latency_ns = callback_start_ns - publish_start_ns`。所有记录绑定 producer monotonic clock、stream 和 exact input hashes，支持离线逐字节重算。这些是成功 exact delivery 的结果投影，不是总体统计或跨 run 性能结论；当前 v9 formal snapshot 已将其纳入 formal chain。

`mw_perf.p0a_contract.v7` 在 v6 基础上将 `capability/communication_latency.json` 升级为 closed、collector-owned `mw_perf.communication_latency.v2`。该 artifact 绑定 registration contract、raw source set、resolution、identity、hook 和 latency sample hashes，冻结 metric 为 `publish_write.start -> final_callback_invocation.start`、公式为 `callback_start_ns-publish_start_ns`、时钟域为 producer monotonic，并证明 sample set 与 hook DeliveryKey set 完全一致。它不生成 min/max/mean/percentile 等未预注册统计；`conclusion=pass` 只表示 `run_scoped_metric_artifact_integrity`。v7 明确限定为 `candidate_run_only`，因此 collector 在创建输出前强制 `candidate_only=true`，root metric 继续为 unsupported 且禁止比较。正式解除 candidate 状态必须使用后续完整合同，不能复用 v7。

`mw_perf.p0a_contract.v8` 在 v7 基础上关闭 collector-owned `mw_perf.communication_delivery.v2` candidate evidence，预注册固定 `publish_count=1000`、`readers_per_sample=1`。collector 从已校验 segment 重建 stage 顺序，并要求四阶段 identity set 完全相等、恰有 1000 个 SampleKey/DeliveryKey 和一个 reader，且 missing、duplicate、out-of-order、status 与 loss 全部为零。exact callback record 的物理顺序是 callback 退出时按 submission/completion-record 排列，不是 callback invocation-entry 顺序；合同同时冻结 `payload_ordinal_claim=none` 和 `transport_identity_events_only`。该 v8 source 在三套 DDS 运行时通过，但仍为不可变 `candidate_only=true`、`P0A_ARTIFACT`；v9 formal snapshot 和 campaign v2 承担正式 closure，不修改该 source。

当前 formal campaign 为 v2，四个 public role 为 v9 formal exact 与三个 v8 companion，并显式保留 v8 exact source。v8 source 保持不可变 `candidate_only=true`/`P0A_ARTIFACT`；v9 formal snapshot 替换重叠 trace_quality/fault/rollback 文档并新增 `formal_anchor`，不回写 v8。campaign v2 验证五个 content-addressed object、trusted contracts、complete declaration closure、exact payload set、archive-derived specialized semantics 和目录外 `expected_campaign_root_sha256`，并通过跨 role anchor/contract/build identity 闭包。rollback fresh build 的外部 dependency 复用限于经内容身份校验的 source，不复用 build object；两个产品 target 的 link plan、response file、object/archive input 与最终 image 均进入封存和独立重验，IBMW object/archive 必须来自 fresh private build root。其 threat model 为 `cooperative_contained_sut_no_active_same_uid`，不提供 active same-UID resistance；该链在三套 DDS 运行时完成 selected capability 的 formal campaign-to-P0a closure。metric 轴仍为 `metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`。

必须区分且分别计数：

| 类别 | 含义 | 对时延统计的处理 |
|---|---|---|
| transport loss | 已要求的 delivery 未到达目标 transport endpoint | 不计时延，delivery correctness 失败/按合同判定 |
| filter/drop | SUT filter、queue、backpressure 或业务策略明确丢弃 | 不计成功时延，按场景合同独立判定 |
| sampling exclusion | delivery 正常但未选入 trace 样本 | 不算 loss，保留采样母数与决策证据 |
| trace loss | 已采样 delivery 可能成功但观测事件丢失/未 finalization | 受影响样本剔除；required 默认必须为 0，例外仅允许显式预注册的非零上限 |

时延分布只统计确认成功、事件完整且被采样的 DeliveryKey；`exact_correlated` 仅断言这些 DeliveryKey 的因果精确性。delivery correctness 是独立门禁：按冻结拓扑核对每个 SampleKey 的预期目标集合、成功/失败/重复/乱序，不能用 correlation ratio 掩盖业务丢失。采样覆盖按预注册母数和选择合同判定；correlation ratio 只衡量已被选中、应被观测的成功 delivery 中有多少形成完整 trace 链，不包含 sampling exclusion。

required 还必须校验 schema、adapter/build/runtime provenance、artifact hash/记录数/finalization、SampleKey 和 DeliveryKey 唯一性、clock、boundary ID、预注册采样覆盖合同、trace loss 合同、freeze 到 measurement seal 的拓扑不变性、controlled teardown 的 generation 完整性与分阶段对账、资源范围和场景合同，以及 canonical `capability/p0a_evidence_manifest.json` 所绑定的 P0a evidence 完整性、identity 链、observer isolation 和 rollback health。缺失/hash 不匹配/identity 链未证/旁路未隔离/回滚健康检查失败均为 `required_stop/invalid`。采用非零 trace loss 上限时，只有实际 loss 不超预注册值且受影响样本已剔除才可保留 run，但报告必须披露 loss 并禁止使用 `100% coverage`；拓扑监测事件丢失不适用该豁免，required 直接 invalid。`compare` 对不同 support level、capability ID、boundary ID、采样策略、trace loss 合同或 clock policy 的 run 禁止计算 B/A、C/A 或 C/B。

## 10. 影响与维护成本

| 模块/候选 | 影响与成本 | 风险控制 |
|---|---|---|
| `tools/mw_perf_collect` | adapter、normalizer、capability freeze、正确性和 compare gate | 保留 producer-owned 路径，invalid evidence 持久化 |
| patched RMW/Fast DDS | 构建、部署、符号/补丁版本矩阵和上游升级维护 | P0a 后才选择；镜像与 build ID 锁定 |
| LTTng/ros2_tracing | 权限、内核/UST 版本、session daemon、trace 存储容量和回收 | doctor preflight、磁盘预算、版本矩阵和 finalization 检查 |
| `src/core/observability` | 独立 comm trace runtime/ring buffer | 与 Core Metrics 解耦，默认关闭 |
| IBMW dispatcher/core observability | internal hook、事件/metadata 穿透、publish/callback boundary 和 topology generation | P0a 先锁一个 ordinary path；与 Core Metrics 解耦，默认关闭 |
| 每 Driver 适配 | 独立 `transport_path_id`、`write_commit`、fan-out 与失败终态 | 每 Driver/path 独立 capability 验收，不能用 `driver_send.returned` 替代 |
| MessageFrame side metadata | 当前 `MessageFrame` 无 token 且 `MessageHandlerTool` 会重建 frame；A 为内部 fixed-width envelope/side metadata，B 为同步单线程 per-call/thread-local token | A 需 runtime overlay/version lock 且可能改变内部 C++ ABI；B 不覆盖异步/跨线程，只能 `stage_only`；两者均须验证 ownership/lifetime/注销/跨线程 |
| Dispatcher/callback | 受控同步路径已用 explicit envelope 到达最终业务 callback；异步 filter/executor 未覆盖 | `callback_enter` 已验证位于 filter 后、最终业务 callback 前；异步路径须独立 patch surface/capability，未证为 `stage_only` |
| DDS/iceoryx extension | 各路径 native identity 和 callback 传递 | capability 独立验收，不修改 payload |
| sidecar ABI | normalizer/correlator 消费接口、schema、ownership、thread、shutdown/finalization 和版本兼容 | sidecar 不进逐消息关联热路径；feasibility 后才决定和冻结 |
| loan lifecycle/finalization | Loan/PublishLoaned/ReleaseLoaned、receiver release、retained callback/release/shutdown quiescence | 独立终态和资源成本；不能把 outstanding loan 当 transport loss |
| A/B/C runner | 通用 trace 与 producer-owned 双轨 | 一致性通过后再迁移主证据 |
| 构建与发布 | observable preset、patched 依赖和多组合制品 | compile-out 仍为生产基线 |

## 11. 分阶段交付

### P0a：Feasibility Spike

- **P0a-core** 当前唯一选定 capability 为 DDS ordinary non-plain、单 Driver、同步 callback。其 formal chain 已在 system DDS、Fast DDS 2.6.12 和 3.2.2 通过，状态为 `valid_for_p0a_completion=true`；Iceoryx ordinary 仍为 `stage_only`。该 completion 轴不改变 metric 轴的 unsupported/invalid，也不支持性能接受、排名或 latency advantage；
- **P0a characterization** 单独记录 Plain loan/fallback、multi-driver、publisher/receiver loan、retained、POD/Iceoryx、异步 filter/executor 及其它路径的矩阵结果。它们不是 P0a-core 通过条件，未完成不阻塞首个 ordinary exact；但每项都是独立 capability，不能由 P0a-core 外推；
- 完成函数级 boundary、调用图和 ROS2/IBMW 等价性证明；
- 选取并验证首个待验证候选组合/构建候选 `Humble + rmw_fastrtps_cpp + Fast DDS 2.6.12 + Fast CDR 1.0.29`；同时如实登记仓库兼容记录中的 Fast DDS 2.6.11（`src/tools/ibmw_gen/ros2_interface_import.py`）。这些是 SDK build lane，不等于 ROS2 运行期实际版本；版本结论不外推到 Jazzy、Fast DDS 3.x 或其它 RMW；
- 以独立 validation overlay/观测镜像加载 patched RMW/Fast DDS 作为正式 exact 首选；`LD_PRELOAD` 仅做低风险 C ABI 诊断，uprobes/LTTng 外部观测仅作 `stage_only` 验证；完全不能替换运行库时明确无 exact；
- 冻结 exact candidate 的完整 patch/overlay surface：Fast DDS write/read/take identity、`rmw_fastrtps` receive/take、`rcl`/`rclcpp` subscription/callback registration 和 executor dispatch metadata 传递；共享库、tracepoint 或 callback wrapper 无法可靠覆盖，或静态链接、inline/hidden wrapper、ABI 不匹配时，候选只能 `stage_only`，required exact 直接停止；
- 验证“业务不改不等于运行环境不变”，记录 `business_unchanged/runtime_changed/measurement_kind/deployment_mode`、实际 maps/SONAME/build-id/SHA 和 resolution；
- 对 characterization 矩阵中的 ordinary/plain/serialized/true-loaned、intra/inter/mixed 和 iceoryx 各路径分别记录 native identity/API/callback 传递结果，不把它们作为 P0a-core 通过条件；
- 分离真实业务 shadow run 与 harness，验证 overlay 真实性、业务 topic/QoS/payload/启动命令不变、无 observer、故障注入、资源预算和可回滚性；
- 旁路 Fast DDS reader 只在隔离 domain/独立 harness 诊断；真实被测 domain 不得创建该新增 endpoint，且记录 `runtime_changed`/`observer_latency` 及 observer isolation evidence；
- 产出并校验 canonical `capability/p0a_evidence_manifest.json` 及其固定 P0a evidence：`capability/resolution.json`、`capability/hook_callgraph.json`、`normalized/identity_trace.jsonl`、`validation/fault_injection.json`、`validation/rollback.json`；实际使用 Fast DDS 旁路/observer 时增加并 required `capability/observer_isolation.json`，否则保留 `not_applicable` entry 和原因。预注册并冻结 `registration_contract_sha256`/`registration_contract_version`、`run_type`、`observer`、`measurement_kind` 及完整 artifact universe；validator 必须先校验 contract hash，再按 contract 判定 status，禁止 manifest 漏报、增删或降级。`raw/comm_trace/` 递归展开为按 UTF-8 路径字节序排列的实际文件 entries，并以 `directory_finalization` entry 校验完整集合。validator 重算每个文件的 hash、record count、finalization 和 status；required/optional 的 0 只有在真实空文件时合法，contract 声明的最小记录数仍必须满足。任一 manifest/entry 缺失、路径漂移、目录集合不完整、hash/record count/finalization/status 不一致、identity 链未证、旁路未隔离或回滚健康检查失败即 `required_stop/invalid`；
- 验证 P0a-core 的 SampleKey/DeliveryKey、单 Driver endpoint epoch、采样覆盖/暂存算法、trace loss 和 clock 合同；characterization 的 ordinary fan-out/multi-driver、loaned first-success-wins 单 path 等另按 capability 记录；
- 验证 `topology_generation/topology_change` 从 freeze 到 finalization 的持续监测、measurement seal、受控 teardown 分阶段对账及监测事件丢失时的 required invalid 行为；
- 完成 ABI inventory，明确并验证 side metadata 选项 A 或 B、ownership/lifetime/跨线程/注销/shutdown；sidecar 候选、部署、LTTng 权限/存储和版本矩阵另行记录；
- 产出逐路径 `exact_candidate/stage_only/unsupported` 结论，不宣称已支持通用时延。

### P0b：Contract/ABI Freeze

- 在 P0a-core DDS ordinary non-plain 单 Driver/同步 callback 通过后冻结 publisher/subscriber endpoint epoch、`uint64` counter、callback registration、每 Driver `transport_path_id`/`write_commit`、已选择的 side metadata A/B 和 patch surface；
- 只冻结 P0a 已证明可行的事件 ABI、DeliveryKey、函数边界、clock、capability、拓扑 generation/change、采样覆盖、trace loss 和 exact patch/overlay surface 合同；具体组合在 maps/SONAME/build-id/SHA/resolution 通过前保持 `not_tested`；
- 分别冻结 `rclcpp`、`rcl/rmw` C API、`rmw_fastrtps` 内部、Fast DDS public/private C++、Fast CDR、compiler/libstdc++/glibc、LTTng ABI 和 uprobe symbol 合同；每个组合登记 `verified_exact/verified_stage_only/known_incompatible/not_tested`；
- 如选择 C sidecar，冻结其 query/version/size/reserved/ownership/thread/shutdown 合同；
- 冻结静态 preflight、readiness 后 capability freeze、measurement topology seal、controlled teardown 分阶段对账、delivery correctness 和 invalid evidence schema；
- 冻结 canonical `capability/p0a_evidence_manifest.json` 顶层不可变 `registration_contract_sha256`/`registration_contract_version`、registration contract 对 `run_type`/`observer`/`measurement_kind`/完整 artifact universe 的绑定、`run_type -> artifact` 权威映射、P0a required/conditional evidence 文件清单及其 `run_id/capability_id/path_id/schema_version/sha256/record_count/finalization/status` 合同；validator 先校验 contract hash，再推导完整 entry 集合，manifest 不得自行降级、增删。同步冻结 `required`、`optional`、`not_applicable` 枚举、not applicable 的 null/zero/finalization/reason 规则、required/optional 实际非负 record count（真实空文件允许 0）、contract 最小记录数门槛、目录递归展开与 `directory_finalization` 集合校验、UTF-8 路径字节序和 JSONL/JSON/binary record count 规则。缺失、路径漂移、目录集合不完整、hash/record count/finalization/status 不一致、required 降级、`not_applicable` 无原因、identity 链未证、旁路未隔离或回滚健康检查失败保持 `required_stop/invalid`；manifest/hash 的检测边界及目录外签名不纳入 P0a 的限制也一并冻结；
- 建立 Core Metrics x Trace 的完整开销基线和 compare measurement gate。

### P1：Runtime 与控制面

- 实现 CLI 编排、LTTng/IBMW trace runtime、ring buffer、topology generation/change 监测、normalizer 和 finalization 对账；
- 实现独立 prefix/镜像、启动前 resolution、显式 `LD_PRELOAD` 清除与原始环境回滚；collector/overlay/LTTng 的 CPU、内存、磁盘和 trace 资源计入预算；
- 实现 required run 的采样覆盖预注册、逐 SampleKey 选择证据、trace loss 默认零/非零上限校验和受影响样本剔除；
- stock ROS2 仅为已验证 ordinary typed inter-process 提供 `stage_only`；
- IBMW 公共 endpoint 注册、publish/callback boundary 和 correctness 证据闭环。

### P2：A/B 已验收精确路径

- 只实现并验收 P0a-core 通过的 ROS2 RMW/DDS 路径/类型类别与 IBMW DDS 对应能力；
- 再扩展 ordinary multi-driver，并分别验收 publisher loan、receiver loan、retained、`plain_loan_write`、`plain_fallback_write`、Iceoryx ordinary/POD-only 等独立 capability；任何能力不因 ordinary 通过而自动获得 exact；
- 仅将版本锁定且 overlay 真实加载、identity 到达最终 callback 的组合标记 exact；preload 诊断、uprobes/LTTng 外部观测、旁路 reader 保持其明确的 stage/diagnostic 定位；
- 对支持 fan-out 的 ordinary 路径，以每目标 DeliveryKey 覆盖 multi-driver；当前 loaned 按 first-success-wins 的单 path capability 单独验收，除非未来另行冻结 multi-loan 合同；两类均与 producer-owned benchmark 双轨验证；
- 对已采样、成功且事件完整的 DeliveryKey 证明因果精确，并通过预注册采样覆盖、topology generation/finalization 对账、clock、trace loss、delivery correctness 和性能门后，才标记该有效 run 的 `exact_correlated`。

### P3：分开的 C 能力

- iceoryx ordinary、serialized loan、retained receive 分别实现和验收；
- POD-only 只有跨进程 `(segment, offset, generation)` 防复用证明通过后才启用；
- C 与 A/B 分开报告 capability 和验收结论，不以“A/B 已通过”代替 C 证据。

### P4：扩展组合

- Cyclone DDS 和其他 IBMW Driver 按独立 capability 验收；
- 多机 Agent/PTP/PHC；
- 趋势和完整配对统计。

## 12. 方案优缺点

### 优点

- 不要求业务修改消息或主动写 latency 文件；
- 以 native identity 和每目标 DeliveryKey 支持 ordinary fan-out/multi-driver，并将当前 loaned 限定为单 path capability，避免启发式错配或能力外推；
- ROS2 与 IBMW 共享主指标，但只有函数边界等价时才允许比较；
- capability、正确性、观测丢失和性能证据均 fail-closed；
- 默认关闭和分档采样兼顾生产性能，统一 CLI 保持可用；
- 按已验收路径扩展，不再笼统承诺任意消息类型或所有零拷贝路径。

### 缺点

- 不是单纯 Python 工具改造，需要 ROS2/RMW/DDS 和 IBMW runtime 共同适配；
- IBMW 选定窄能力的 formal P0a completion 已由 validated chain 证明；ROS2/RMW formal exact、通用路径、异步/multi-handler/multi-driver、其它类型与 transport 仍未完成，首版其它能力仍可能只有 `stage_only` 或 `unsupported`；
- patched RMW/Fast DDS、LTTng 和 sidecar 带来部署、存储、ABI 与版本矩阵维护成本；
- endpoint/callback identity 穿过 executor 和异步 filter 的实现复杂；
- POD-only 零拷贝和跨主机单向时延可能长期 unsupported。

## 13. 审核决策点

请在实施前确认：

1. 正式主指标采用同 DeliveryKey、同 boundary ID 的 `publish_enter -> target callback_enter`；
2. 接受 Hybrid，不以 Generic Observer、stock tracing 或 DDS Statistics 作为正式主路径；
3. 当前唯一选定 capability 为 DDS ordinary non-plain + single driver + synchronous callback；其 formal P0a completion 已通过，但 v8 source 仍保持 candidate-only/P0A_ARTIFACT，metric 仍 unsupported/invalid。不得外推到 Fast DDS/RMW 的其它路径、类型、transport 或 ROS2 formal exact；
4. 接受 endpoint 注册对象、uint64 counter、epoch 和每目标 delivery correctness 合同；
5. 不修改 payload；POD-only 未证明 `(segment, offset, generation)` 防复用时保持 unsupported；
6. 接受 P0a feasibility -> P0b contract/ABI freeze -> P1 runtime/control -> P2 A/B -> P3 分开 C 能力 -> P4 的顺序；
7. 初期编译默认 OFF、运行默认 off，并按 Core Metrics x Trace 完整矩阵预注册性能门；
8. 首版限定 Linux 单机跨进程，跨机只有完整 PTP/PHC 与误差界合同后再支持；
9. required 在静态 preflight 或 readiness 后 freeze 任一阶段不满足即在 warmup/sample 前停止，并保留 invalid evidence；
10. `exact_correlated` 只描述已采样、成功且事件完整 DeliveryKey 的因果精确性；run 有效性另由预注册采样覆盖、trace loss、正确性、拓扑和 finalization 合同判定；
11. required 默认 trace loss = 0；允许非零时必须预注册上限、剔除受影响样本并禁止声称 `100% coverage`；
12. 从 freeze 到 finalization 持续监测 `topology_generation/topology_change`；measurement seal 前拓扑变化使 required invalid，seal 后仅允许可完整对账的预注册 controlled teardown；
13. 当前 loaned 是 first-success-wins 的 `source_driver` 单路径能力，不支持 multi-driver fan-out；P2 的 multi-driver 覆盖只适用于支持 fan-out 的 ordinary 路径，未来 multi-loan 需另立合同；
14. delivery correctness、sampling exclusion、trace loss 和 correlation 各自判定，不相互掩盖。
15. 首个 P0a-core 推荐组合为 DDS ordinary non-plain + single driver + synchronous callback；理由是对标 Fast DDS native identity 风险最高、语义单一。待验证候选构建仍为 `Humble + rmw_fastrtps_cpp + Fast DDS 2.6.12 + Fast CDR 1.0.29`，并如实登记仓库兼容记录中的 Fast DDS 2.6.11（`src/tools/ibmw_gen/ros2_interface_import.py`）；二者均是 SDK build lane，不等于 ROS2 运行期实际版本。maps/SONAME/build-id/SHA/resolution 通过前保持 `not_tested`，之后才可登记具体 `verified_*`，结论不外推到 Jazzy、Fast DDS 3.x 或其它 RMW；
16. 正式 exact 首选隔离 validation overlay/观测镜像的 patched RMW/Fast DDS，但必须冻结并验证 Fast DDS write/read/take、`rmw_fastrtps` receive/take、`rcl`/`rclcpp` subscription/callback registration 和 executor dispatch metadata 传递的完整 patch/overlay surface；共享库、tracepoint 或 callback wrapper 无法可靠覆盖，或静态链接、inline/hidden wrapper、ABI 不匹配时只能 `stage_only`/required stop；
17. 接受业务源码、二进制、IDL、payload 不变不等于运行环境不变，报告必须包含 `business_unchanged`、`runtime_changed`、`measurement_kind` 和 `deployment_mode`；
18. 真实业务 shadow run 与 harness 分离，必须证明实际库加载、业务 topic/QoS/payload/启动命令不变、无 observer、故障注入结果和 evidence fail-closed；
19. 版本组合逐一登记 `verified_exact`、`verified_stage_only`、`known_incompatible` 或 `not_tested`，不以 distro 或 API 名称推断 ABI；
20. validation 使用独立 prefix/镜像，启动前完成 resolution，显式记录并清除 `LD_PRELOAD`，失败回滚到原始环境，collector 和观测资源纳入预算。
21. Fast DDS 旁路 reader 只允许在隔离 domain/独立 harness 诊断；真实被测 domain 的新增 reader 必然影响 discovery/matching、ACK/NACK、history/resource 和路径，必须标记 `runtime_changed`、`measurement_kind=observer_latency`，不得进入正式 measurement，也不得声称“不改变 matching”。
22. 所有 run 必须通过 canonical `capability/p0a_evidence_manifest.json` 预注册并绑定不可变 `registration_contract_sha256`/`registration_contract_version`、`run_type`、`observer`、`measurement_kind` 及完整 artifact universe；validator 必须先校验 contract hash，再按 contract 推导 entry，拒绝漏项、增项、删除、错误降级或 required 改为 optional/not_applicable。P0a run 绑定五项 required evidence，Fast DDS 旁路/observer 实际使用时还必须绑定 `observer_isolation.json`，未使用时保留 `sha256=null`、`record_count=0`、`finalization=not_applicable` 且带原因的 entry。stage_run 必须登记五类采集/结果 artifact，启用为 required、明确禁用为 not_applicable；exact_run 的全部预注册采集/结果 artifact 和五项证明均 required；observer_diagnostic 必须是 `observer_latency` 且 observer isolation required。`optional` 仅能由 registration contract 明确声明。所有 entry 和文件引用相同 `run_id/capability_id/path_id`，validator 重算每文件 `sha256/record_count/finalization/status`，required/optional 的 record_count 为真实非负整数（真实空文件可为 0，不能以 0 占位；contract 最小记录数仍必须满足），并按 JSONL 物理行、JSON schema 记录定义、binary 预注册规则计数；任一 manifest/路径/集合/hash/record count/finalization/status 不一致或 `not_applicable` 无原因即 `required_stop/invalid`。该 manifest/hash 只检测缺失、错配、陈旧和非恶意采集后变化，不抵抗结果目录写权限持有者的主动篡改；目录外签名/远端证明不纳入 P0a。
23. 历史决策记录/尚待评审的剩余事项：当前唯一选定 capability 为 DDS ordinary non-plain、single driver、synchronous callback，并接受选项 A 的 internal fixed-width envelope/side metadata；其 formal P0a completion 已通过。剩余事项是 metric/性能门、ROS2/RMW formal exact，以及异步 API/context executor、Plain loan/fallback、multi-driver、loan/retained/POD/Iceoryx 和其它类型/transport 的独立 capability 边界。Iceoryx ordinary 首版保持 `stage_only`。

## 14. ROS2 依据与限制来源

- ROS2 Humble tracing 教程：<https://docs.ros.org/en/humble/Tutorials/Advanced/ROS2-Tracing-Trace-and-Analyze.html>
- `ros2_tracing` Humble：<https://github.com/ros2/ros2_tracing/tree/humble>
- ROS2 消息流分析论文：<https://arxiv.org/abs/2204.10208>
- `ros2_tracing` 论文：<https://arxiv.org/abs/2201.00393>
- Fast DDS 2.6 Statistics topics：<https://fast-dds.docs.eprosima.com/en/v2.6.10/fastdds/statistics/dds_layer/topic_names.html>
- `LD_PRELOAD` 动态链接器说明：<https://man7.org/linux/man-pages/man8/ld.so.8.html>
- `rmw_fastrtps`：<https://github.com/ros2/rmw_fastrtps>
- Fast DDS 版本与兼容性文档：<https://fast-dds.docs.eprosima.com/en/latest/notes/versions.html>
- Linux uprobes 文档：<https://docs.kernel.org/trace/uprobetracer.html>

这些资料支持阶段 tracing 和 DDS 内部统计，但不自动提供任意 topic 到目标业务 callback 的逐消息因果关联。**仅对本文选择的 native DDS identity 路线**，RMW/Driver correlation instrumentation 将 native identity 传到 target callback dispatch 是获得 exact E2E 的必要条件；这不是对所有可能观测方案在逻辑上的普遍必要条件。通用 exact 仍未实现；当前增加的 exact artifact 只覆盖预注册的 ordinary non-plain、single Driver、single synchronous callback capability。
