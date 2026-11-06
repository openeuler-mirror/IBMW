<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# IBMW Doctor：Topic 检查

`ibmw doctor` 通过原有 CLI provider 机制执行诊断。当前只检查
**有发布者但没有观察到同名、同类型订阅者**的 topic；暂不检查 transport
断连、module 健康、QoS 兼容性和日志级别。

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

默认输出告警和未完成检查，再输出汇总，例如：

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

汇总按来源内的 topic/type 分组计数，不是业务模块数。不同发现来源、domain、
观察者配置以及逻辑名/原始 wire 名分别统计，不把它们合并成一个成功结果。
重复的端点身份只计数一次。来源部分失败时，保留其他来源的独立检查结果。

- 退出码 **0**：检查完成；默认告警不使命令失败，空图也不算故障。
- 退出码 **1**：使用了 `--include-warnings`，且发现告警。
- 退出码 **2**：参数错误或检查未完成；优先于告警退出码。
- 命令进入 doctor 前的 CLI 配置/插件加载错误沿用原有 host 的非零退出码。

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
