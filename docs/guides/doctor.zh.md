<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# IBMW Doctor：Topic、QoS、模块生命周期与日志级别检查

`ibmw doctor` 通过原有 CLI provider 机制执行诊断，包括
**有发布者但没有观察到同名、同类型订阅者**的 topic，以及 **DDS/ROS 2
端点之间的 QoS 策略兼容性和本机配置一致性**、**已记录的模块初始化和启动
失败**，以及**运行时日志级别与明确预期值的偏差**。`ibmw doctor` 默认调度
全部已实现检查；`ibmw doctor module` 只检查模块生命周期结果，
`ibmw doctor log` 只检查日志。子命令和根命令共享对应的查询、判断和输出实现。
MQTT、Zenoh 等后端不做 QoS 检查；transport 断连、模块运行中业务健康尚未实现。

## 使用

```bash
ibmw doctor
ibmw doctor --report
ibmw doctor --include-warnings
ibmw doctor --report-failed
# 先将 IBMW_REGISTRY_PATH 设为运行时使用的私有注册根目录（当前用户所有，权限 0700）。
ibmw doctor --registry-path "$IBMW_REGISTRY_PATH"
ibmw doctor --dds --domain 10 --timeout 3
ibmw doctor --ros2 --ros-domain 10 --timeout 3
ibmw doctor --zenoh --zenoh-domain robot_a
ibmw doctor --registry --dds --domain 10
ibmw doctor --http 127.0.0.1:50080 --expect-level CameraModule=INFO --report
ibmw doctor module --report
ibmw doctor module --runtime robot_a --include-warnings
ibmw doctor log --dds --domain 10 --expect-level CameraModule=INFO --report
```

发现参数与 `ibmw topic list/info` 共用实现：默认只查看本机 registry；
显式选择 DDS、ROS 2 或 Zenoh 后只查询所选来源，需要本机 registry 时添加
`--registry`。`--all` 表示四种发现来源，不表示扫描全部 domain。
`-r` 仍是 `--ros2`，报告使用完整的 `--report`。
显式 `--domain`/后端专用 domain 参数及 `ROS_DOMAIN_ID` 的优先级与 topic 命令相同。

默认忽略隐藏 topic，使用 `--include-hidden-topics` 纳入检查。
根命令的 `--timeout` 为网络发现观察窗口（默认 2 秒，范围 0.01～60 秒）；
显式提供时也作为日志 RPC 响应超时。不提供时日志 RPC 使用原有 5 秒默认值。
该参数不控制模块查询；模块查询复用 Runtime Agent 客户端原有的 socket 超时。
这不是对所有后端初始化/退出过程的硬性总超时；本机 registry 为即时快照。
网络发现会创建临时观察者并产生发现流量，但不创建业务 publisher/subscriber，
不会因 doctor 自己订阅而掩盖无订阅者问题。

## 输出与退出码

默认输出告警和未完成检查，再输出汇总。例如，topic 检查部分：

```text
[WARN] topic "/camera/image" source=dds domain="10" type="example/Image" publishers=1 subscribers=0
  Publisher without subscriber observed in this source for this topic/type.
Topic checks: 0 OK, 1 WARN, 0 UNKNOWN, 0 SKIP
```

`--report` 输出全部结果、观察来源/时间和处理建议；`--report-failed` 只展开
告警及未完成检查，并附观察范围。两者不能同时使用。报告选项只改变呈现，
与普通检查使用同一份快照及退出码规则。

| 状态 | 意义 |
| --- | --- |
| OK | 观察到了同一来源、同名同类型的发布者和订阅者；不证明 QoS 兼容或消息可达 |
| WARN | 观察到了发布者，没有观察到对应订阅者 |
| UNKNOWN | 发现失败、后端不可用，或端点元数据不足，无法完成检查 |
| SKIP | 没有可检查的发布者；包括空图和只有订阅者的 topic |

上述表格解释的是 `topic` 行。`qos` 行按端点/端点配对的每项策略分别报告：
`OK` 为该项检查通过，`WARN` 为已发现不兼容或配置矛盾，`UNKNOWN` 为缺少
判断依据，`SKIP` 为不适用、不参与匹配或该来源不提供本机配置。
`--report` 展示所有策略及跳过原因；普通输出和 `--report-failed` 展示告警和
无法判定的项目。`Topic checks` 和 `QoS policy checks` 分别计数；后者不是
连接数或 topic 数。同一配对同时存在多项问题时全部报告。

汇总按来源内的 topic/type 分组计数，不是业务模块数。不同发现来源、domain、
观察者配置以及逻辑名/原始 wire 名分别统计，不把它们合并成一个成功结果。
重复的端点身份只计数一次。来源部分失败时，保留其他来源的独立检查结果。

- 退出码 **0**：没有 `UNKNOWN` 或模块查询错误；默认告警不使命令失败，空图也不算故障。
- 退出码 **1**：使用了 `--include-warnings`，且发现告警。
- 退出码 **2**：参数错误或检查未完成；优先于告警退出码。
- 日志 RPC 被 SIGINT/SIGTERM 中断时保留退出码 **130/143**。
- 命令进入 doctor 前的 CLI 配置/插件加载错误沿用原有 host 的非零退出码。

根命令分别输出 `Topic checks`、`QoS policy checks`、`Module lifecycle checks`、
`Log level checks`，最后输出 `Doctor checks` 的总告警与未完成检查数。
模块查询错误另列 `module query errors`，不计入模块告警或 `UNKNOWN`。
某个查询失败不会抹掉已经获得的其他检查结果；有 `UNKNOWN` 或模块查询错误时
总退出码为 2。

## 模块生命周期检查

```bash
# 全部检查；模块部分读取本机 Runtime Agent 的生命周期记录
ibmw doctor --report

# 只检查模块，不执行 topic/QoS 发现或日志 RPC
ibmw doctor module --report
ibmw doctor module --registry-path "$IBMW_REGISTRY_PATH" --runtime robot_a --include-warnings
ibmw doctor module --report-failed

# topic 同时检查 registry 和 DDS，模块仍从指定本机 registry 查询
ibmw doctor --registry --dds --registry-path "$IBMW_REGISTRY_PATH" --report
```

实现先通过只读 `InspectLocalRuntimes()` 获取本机有效运行时，再调用现有
`RuntimeGraphAgentClient::ListModuleLifecycle()`。不会调用模块回调，也不修改
`ibmw module list/info` 的行为或输出。

每个模块分别检查两个已记录阶段：初始化（访问接口配置及受管模块的 `Setup()`）
和启动（受管模块的 `Run()`）。`failed` 为 `WARN`，输出失败阶段和记录中的原因；
`succeeded` 为 `OK`。尚未执行、正在执行的阶段为 `SKIP`，不推断卡死。
app context 没有受管 `Run()` 回调，其启动阶段为不适用的 `SKIP`。
记录的时间戳是最后一次阶段变化的时间，不是心跳；成功不代表当前业务健康。

```text
[WARN] module runtime="robot_a" pid=1234 module="CameraModule" kind=managed phase=start
  Recorded start failed. Reason: Module Run() returned false.
Module lifecycle checks: 1 OK, 1 WARN, 0 SKIP
```

默认输出 `WARN` 和带原因的 `SKIP`；`--report` 还输出 `OK`、记录时间戳和范围；
`--report-failed` 仅输出 `WARN`。汇总按阶段计数，不是模块数。无有效运行时或
返回空记录时输出覆盖范围的 `SKIP`，不表示全部模块检查通过。默认告警退出码为 0，
加 `--include-warnings` 后为 1。

读取失败、不支持查询或响应数据无效时，向 stderr 输出普通 CLI 查询错误，
退出码为 2；不输出 `[UNKNOWN] module`，也不将查询问题判定为模块故障。
其他运行时的记录仍会继续检查。显式指定的 `--runtime` 不存在时同样是查询错误。

模块检查始终使用本机 registry，不随 topic 网络发现后端改变。`--runtime`
仅筛选模块检查；根命令已有的 `--module` 仍只筛选日志 logger。
根命令沿用 topic 参数规则：只选网络发现时不能单独传 `--registry-path`，需同时
选择 `--registry` 或 `--all`；模块专用子命令可直接传 `--registry-path`。

查询依赖已开始服务的 Runtime Agent。当前 Engine 顺序中，初始化或启动失败
可能发生在 Agent 开始服务之前，此时记录只能由宿主通过进程内接口读取，doctor
无法取得；本次不增加落盘或启动失败后的服务机制。进程退出后也不能查询这些
内存记录。检查不涵盖业务异常、心跳、卡死或“预期模块未出现”。完整的基础接口
与记录范围见[模块生命周期记录](user-guide.zh.md#模块生命周期记录基础接口)。

## 日志级别检查

```bash
# 全部检查：topic/QoS 默认使用本机 registry，日志通过指定 HTTP 服务查询
ibmw doctor --http 127.0.0.1:50080 --expect-level CameraModule=INFO --include-warnings

# 只检查日志；连接参数沿用 ibmw log list 的 RPC 逻辑
ibmw doctor log --dds --domain 10 --report
ibmw doctor log --http 127.0.0.1:50080 --module CameraModule --expect-level CameraModule=INFO

# 每个 logger 可以有不同的要求；级别不区分大小写
ibmw doctor log --http 127.0.0.1:50080 \
  --expect-level CameraModule=INFO --expect-level core=WARN --report

# topic 检查 DDS/ROS 2，日志选择另一个明确的 RPC 服务
ibmw doctor --dds --ros2 --domain 10 --http 127.0.0.1:50080 \
  --log-backend http --service robot_a/logs --report
```

日志查询复用 `QueryLogLevels()` 和已有 `GetModuleLogLevel` RPC，只读取选定
服务所属运行时已登记 logger 的实际阈值，不调用 `SetModuleLogLevel`。
不修改 logger、启动配置或日志文件，也不分析日志正文是否包含 ERROR。
查询成功不代表日志一定写入控制台或文件；输出后端还可能有额外过滤条件。

支持现有日志 RPC 的 DDS、HTTP、gRPC、MQTT 和 Zenoh 后端，具体可用性取决于
构建和部署。这里的 MQTT 仅作为已有日志查询的传输通道，不新增 MQTT QoS
诊断。ROS 2 图和本机 registry 不直接提供日志查询接口或可靠的日志服务地址。
日志服务部署和多实例命名参见 [日志命令](log-control.zh.md#服务选择和部署)。

日志连接的选择规则：

- 只选择一个支持的 RPC 后端时直接复用。例如 `--dds --domain 10` 同时用于
  DDS topic/QoS 发现和日志 RPC，日志服务默认名称与 `ibmw log list` 相同。
- 多个日志 RPC 后端同时选中时，使用 `--log-backend` 选其中一个；未指定则
  日志检查为 `SKIP`，明确提示目标不唯一。不会依次尝试多个服务或合并实例。
- 根命令的 `--all` 仍表示四种 topic 发现来源，并为日志提供 DDS/Zenoh 候选；
  可以用 `--all --log-backend dds` 明确选择日志 RPC。
- 无参数 `ibmw doctor` 执行四个检查类别。没有日志连接信息时输出日志
  `SKIP` 和原因；已经选定后端但服务不可达、缺少扩展或响应不可信时为 `UNKNOWN`。
- `--service` 指定日志服务基础名称；`--module` 将日志检查限定到一个 logger。
  这些参数不改变 topic/QoS 范围。一个服务只代表其所属运行时，不代表整个域。
- 日志专用子命令的 `--timeout` 沿用 RPC 响应超时（默认 5 秒，0.01～3600 秒）；
  两个入口的 `--discovery-timeout` 都表示日志客户端就绪超时（默认 2 秒）。

`--expect-level module=level` 可重复指定不同模块；重复指定同一模块、非法
级别，以及指定 `--module` 后为其他模块设置预期值均为参数错误，在查询前拒绝。
有效级别为 TRACE、DEBUG、INFO、WARN、ERROR、FATAL、OFF。
比较采用相等判断，不假设所有模块都应当是 INFO，也不把 DEBUG/OFF 本身视为故障。
当前只使用命令行明确给出的预期值，不自动读取 YAML 或推断启动时的级别。

```text
[WARN] log_level source=dds target="domain=10" service="ibmw.protocols.log_control_extension.LogControlService" module="CameraModule" actual="DEBUG" expected="INFO" baseline=command_line
  Actual log level differs from the explicit expectation; confirm whether this is intentional debugging.
Log level checks: 0 OK, 1 WARN, 0 UNKNOWN, 0 SKIP
```

日志结果按 logger 计数：匹配明确预期值为 `OK`，偏差为 `WARN`；未返回所要求的
logger、返回非法级别/字段、查询失败为 `UNKNOWN`，不据此断言模块故障或 transport
断连。无预期值时展示实际级别并将比较标为 `SKIP`；空服务也为 `SKIP`。
无法完成整个请求时计一个查询级别的 `UNKNOWN`，不输出不可信的部分结果。
默认输出日志 `WARN`、`UNKNOWN` 和带原因的 `SKIP`；`--report` 还显示 `OK`，
`--report-failed` 仅显示 `WARN`/`UNKNOWN`。没有预期值的 `SKIP` 不算检查通过。

## QoS 策略覆盖

命令和发现参数保持不变，例如：

```bash
ibmw doctor --dds --domain 10 --report --include-warnings
ibmw doctor --ros2 --ros-domain 10 --report --include-warnings
```

检查复用同一份发现快照，不创建业务订阅者，不修改端点 QoS。所有发现结果
都是观察窗口内的状态；策略兼容不能证明消息已经送达。

例如，发布端 `best_effort` 无法满足订阅端 `reliable` 时：

```text
[WARN] qos topic="/camera/image" source=ros2 domain="10" instance="" driver="ros2" type="example/Image" policy=reliability
  publisher="..."
  subscriber="..."
  offered="best_effort" requested="reliable"
  Offered policy does not satisfy this subscriber.
```

同一 topic 有多个订阅者时逐一检查，已兼容的订阅者不会掩盖其他不兼容端点。

| 策略 | DDS | ROS 2 |
| --- | --- | --- |
| reliability | offered >= requested；best_effort 不能满足 reliable | 同左 |
| durability | volatile / transient_local / transient / persistent 有序比较 | volatile / transient_local 有序比较 |
| deadline | offered period <= requested period | 同左，保留 RMW 默认时长标记的语义 |
| liveliness | automatic / manual_by_participant / manual_by_topic 有序比较 | automatic / manual_by_topic 有序比较 |
| liveliness_lease_duration | offered <= requested | 同左 |
| ownership | shared/exclusive 必须相同 | 不适用 |
| partition | 默认分区及双向 POSIX 通配匹配 | 使用 ROS 图中的 topic 名称 |
| disable_positive_acks | 读端不能关闭写端仍要求的 ACK | 不适用 |
| data_representation | Fast DDS 3 按写端首选编码与读端支持集合比较；Fast DDS 2.6 未在端点匹配中执行，明确 SKIP | RMW 图接口未暴露独立策略，不套用 DDS 规则 |
| type_consistency、topic_kind | 比较可观察的类型标识、强制验证条件和 keyed/unkeyed；缺少依据时 UNKNOWN | 使用 ROS 图报告的 topic/type 配对 |
| history、depth | 读取时验证 KEEP_LAST 的正深度；网络 SEDP 不提供时 SKIP | 验证观察到的 history/depth；默认、未知值明确无法判定 |
| lifespan | 验证写端时长；不把读写差异当作匹配故障 | 同左 |
| resource_limits | 本机可获得时检查上限及 history/depth 的一致性 | RMW QoS profile 无此字段 |
| ownership_strength | 检查取值并展示；强度不同不是不兼容 | 不适用 |
| presentation、destination_order、latency_budget、time_based_filter、durability_service、transport_priority | 在支持的 Fast DDS 2.6/3.2 匹配实现中不执行，明确 SKIP | 不适用 |
| user/topic/group data、entity_factory、读写生命周期、publish_mode、data_sharing | 本机行为或描述信息，不按两端相等判断；明确 SKIP | 不适用 |
| reliability_max_blocking_time、liveliness_announcement_period、disable_positive_acks_duration | 可获得时检查本机时长，不参与两端匹配 | RMW profile 不暴露这些字段 |
| avoid_ros_namespace_conventions | 不适用 | 命名选项，展示适用范围；不比较布尔值判断 QoS 不兼容 |

Fast DDS 规则以观察器构建所用的原生匹配实现为依据，不代表识别了远端 DDS
厂商或版本。Fast DDS 2.6 与 3.2 的类型检查均有严格匹配限制，不承诺通用
XTypes 类型转换。Fast DDS 3 比较完整/最小类型标识；Fast DDS 2 优先完整标识，
再使用最小/旧标识，标识不同时还需要 TypeObject 结构一致性检查，仅有指纹的
快照返回 `UNKNOWN`，不会直接把它当作类型不兼容。缺少 TypeObject/标识时按
该原生实现的类型名回退条件处理。无法还原的结构信息返回 `UNKNOWN`，不靠
同名就宣称结构相同。类型一致性子选项也单独展示适用范围。

时间按秒和纳秒比较，不使用浮点数，也不把缺失字段补零。`infinite`、ROS 2
`default`、未知/缺失分别处理；不把默认枚举值当作已解析的实际策略。

registry 按每个 driver 的 QoS 分别检查。更新后的 DDS/ROS 2 runtime 从已创建
实体取得实际 QoS，并报告 domain；旧 runtime 的注册信息可能不完整，此时
明确 `UNKNOWN`。本机 registry 未提供完整原生类型信息时，使用 `--dds` 进行
网络检查可获取更多类型元数据。不同 domain 或已知不同原生 topic 的端点
不会作为同一配对；缺少 domain 时只比较已知配置并报告范围不完整。
本机 DDS 查询也覆盖 serialized 端点，若同一逻辑 topic/type 对应多个不同配置，
现有查询接口不能指定具体端点，此时返回缺失信息，由 doctor 报告 `UNKNOWN`；
可改用网络发现逐端点检查。非匹配策略在可获取时展示值，user/topic/group data
只展示字节数，不输出其二进制内容。

没有候选发布者/订阅者配对时，兼容性检查 `SKIP`，仍可检查已有端点的本机
配置。不同缓存深度、历史策略、lifespan 或 ownership strength 不直接产生
“不兼容”告警。deadline missed、liveliness lost、消息实际过期等运行事件
需要运行状态或流量证据，不属于配置快照检查。

## 判断范围

无订阅者表示“在所选来源和观察窗口内没有观察到”，并非证明整个网络上绝无
订阅者。默认 registry 只能看到登记到同一本机目录的有效 runtime，无法据此
判断远端 DDS/ROS 2 等订阅者；检查网络端点应选择对应来源。
registry 内的计数表示登记信息，不判断各端点实际 driver、路由或连通性。
只有订阅者没有发布者不属于当前这一步的检查范围。

registry 检查复用原有身份、PID 启动标识、权限、协议及 socket 验证，
忽略原有查询不接受的记录；不诊断 registry 文件损坏本身。
doctor 不删除失效注册文件/socket、不创建缺失目录、不加载业务模块，
原有 `topic`/`module` 等命令的 registry 清理行为保持不变。

`libibmw_doctor_cli.so` 随 runtime 构建并加入自动生成的 `cli.yaml`；安装时放入
`bin/`。自定义 CLI 清单需按现有格式添加该 provider。无需新增后台守护进程。
