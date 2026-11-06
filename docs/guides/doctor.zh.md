<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# IBMW Doctor：Topic 与 QoS 检查

`ibmw doctor` 通过原有 CLI provider 机制执行诊断，包括
**有发布者但没有观察到同名、同类型订阅者**的 topic，以及 **DDS/ROS 2
端点之间的 QoS 策略兼容性和本机配置一致性**。MQTT、Zenoh 等后端不做
QoS 检查；transport 断连、module 健康和日志级别不属于当前范围。

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
```

发现参数与 `ibmw topic list/info` 共用实现：默认只查看本机 registry；
显式选择 DDS、ROS 2 或 Zenoh 后只查询所选来源，需要本机 registry 时添加
`--registry`。`--all` 表示四种发现来源，不表示扫描全部 domain。
`-r` 仍是 `--ros2`，报告使用完整的 `--report`。
显式 `--domain`/后端专用 domain 参数及 `ROS_DOMAIN_ID` 的优先级与 topic 命令相同。

默认忽略隐藏 topic，使用 `--include-hidden-topics` 纳入检查。
`--timeout` 为网络发现观察窗口（默认 2 秒，范围 0.01～60 秒），不表示
对所有后端初始化/退出过程的硬性总超时；本机 registry 为即时快照。
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

- 退出码 **0**：没有 `UNKNOWN`；默认告警不使命令失败，空图也不算故障。
- 退出码 **1**：使用了 `--include-warnings`，且发现告警。
- 退出码 **2**：参数错误或检查未完成；优先于告警退出码。
- 命令进入 doctor 前的 CLI 配置/插件加载错误沿用原有 host 的非零退出码。

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
