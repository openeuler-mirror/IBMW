# OHOS DDS 与 ROS 2 最小依赖互通指南

本文是 IBMW 的 **canonical 中文配套指南**：先说明“做什么、最小到哪里、现有证据证明了什么”，再把可复制的命令交给 sibling 工程中的详细操作文档。第一次接触本工程时，应先读本文，不要从零散的进度文档推断当前能力。

## 1. 一句话目标

在 Linux 主机上使用 OHOS SDK **交叉编译** AArch64 目标程序；目标程序直接使用 Fast DDS 和预生成的 ROSIDL/FastRTPS typesupport，按 ROS 2 Humble 的 topic、type、QoS 与 CDR 约定发送 DDS/RTPS 数据，从而与标准 ROS 2 peer 实现 **DDS wire 兼容**。

这里的四个概念不要混在一起：

- **OHOS 交叉编译**：生成可在 OHOS AArch64 上装载的 ELF，而不是把主机 `/opt/ros/humble` 的库复制过去。host 上运行生成器，target toolchain 只编译目标代码；host/target prefix 必须隔离。
- **DDS wire 兼容**：双方在网络上的 topic 名、DDS type name、CDR 表示、QoS 和版本组合相容。能编译不等于能互通；单个类型互通也不能外推到所有类型、service/action 协议或所有 DDS 版本。
- **ROS 2 最小运行依赖**：产品端不运行 ROS node、executor 或 graph，而是直接调用 Fast DDS。它比完整 ROS 2 小，但不是“零 ROS 库”。
- **消息全面预生成/预编译**：在构建/发布阶段把选定 catalog 的消息 C/C++ 代码与 typesupport 全部生成并编译，产品现场不再运行 ROSIDL generator。这是“静态确定消息集合”的含义，**不等于所有 ELF 都采用静态链接**；实际产品仍可携带受审计的 `.so`。新增或修改消息、生成器、DDS/CDR 或 ABI profile 后必须重新生成、重编译、重验。

## 2. “最小”依赖的准确边界

目标产品运行闭包中：

- **不得有** `rcl`、`rclcpp`、`rclpy`、`rmw_fastrtps_cpp`；
- **可以有且通常需要** ROSIDL runtime/typesupport、`rcutils`、受控的基础 `librmw`，以及 Fast DDS、Fast CDR、消息生成库和 FastRTPS typesupport；
- 基础 `librmw` 是 typesupport ABI/接口闭包的一部分，不是 `rmw_fastrtps_cpp` 这类 RMW implementation；
- 主机或板内的完整 ROS 2 CLI 只作为外部互通 peer，不属于最小产品依赖。

因此，“rcl-free / 无 RMW implementation 产品依赖”是准确说法；“完全不含 ROS 库”不是。

## 3. 三种交付层级必须分开

| 层级 | 包含内容 | 能说明什么 | 不能说明什么 |
| --- | --- | --- | --- |
| substrate-only | OHOS toolchain、Fast DDS/Fast CDR、基础 ROSIDL/runtime 闭包 | DDS 基座和目标架构构建可用 | 没有具体消息产品，也不证明 ROS 2 wire 互通 |
| 单消息 `std_msgs/msg/String` | substrate + String 生成物/typesupport + 薄 adapter/consumer | 最窄、适合新手的真实标准消息构建与双向互通路径 | 不能代表全 catalog、generic G3/G4、service/action wire |
| 全量静态消息产品 | 锁定 catalog 中全部消息预生成/预编译、按 bundle 交付并审计 | 面向产品的固定消息 SDK | 仍不自动提供 node/graph/executor，也不自动实现 ROS service/action 请求应答协议 |

Sibling 仓库中的 `IBMW_together/ros2_minimal_deps/docs/GOALS_AND_PROGRESS.md`
记录了产品 catalog 的 **27 个 interface package / 530 个生成消息类型**。这是全量
产品工作的独立范围与进展，**不能说成已由本文的 `std_msgs/String` 历史验证快照
覆盖**；该快照只覆盖 fresh build-only 和单消息实机互通。

## 4. 本次项目验证摘要（历史 validation snapshot）

以下内容是项目在特定输入和环境下完成过的历史 validation snapshot，用于说明已验证能力的边界，不是用户当前 SDK、源码、制品或网络环境的 evidence，也不引用未入库文件。

| 项目 | 本次项目验证结果 | 已证明 | 未证明或限制 |
| --- | --- | --- | --- |
| fresh OHOS cross build | 通过 | Humble `std_msgs/String` adapter、consumer 与 clean external consumer 的全新 OHOS AArch64 构建；G2 build 成立；ELF 审计未发现被禁止的 `rcl`/`rclcpp`/`rclpy`/`rmw_fastrtps_cpp` | 这是 build-only 结果，未由该项验证目标端执行；不覆盖 G3/G4 |
| 物理双向互通 | 通过 | stock ROS 2 Humble 与 OHOS `std_msgs/String` 在物理网络上完成双向 typed communication | 只覆盖当次单消息、版本组合和应用层结果；不覆盖其他消息、service/action 或所有 DDS 组合 |
| generic G3 / generic G4 | 未完成 | 无 | 不得据上述两项推断完成 |
| pcap / packet decode | 未完成 | 无 | 只能声明应用层 typed interop，不声明报文级解码通过 |

用户必须为自己的每次运行生成 evidence，且至少满足以下规则：

- evidence 由实际命令和输出生成并保存在用户自己的受控工作区；本文不把未入库文件、口头记录或历史快照当作当前运行的证明；
- 记录 SDK/toolchain、源码 revision、消息定义、DDS/CDR 与 ROS 2 profile、构建参数、制品 manifest/hash、目标架构和依赖审计；
- 互通运行还要记录实际使用的 host/target 接口与地址、`ROS_DOMAIN_ID`、topic/type/QoS、两端身份、唯一 payload、stdout/stderr，以及是否具备 packet capture/decode；
- build-only、板端启动、单向通信、双向通信和 packet decode 必须分别标注，未执行项写明未验证；
- SDK、源码 revision、消息定义、DDS/CDR 版本、toolchain、制品或网络配置变化后，必须重新生成 evidence，不能沿用历史结论。

## 5. 小白推荐流程

1. **先选层级**：第一次只做 `std_msgs/String`，不要直接把全量 530 类型、service/action 或 generic G3/G4 当作起点。
2. **固定输入**：记录 OHOS SDK、toolchain、Humble profile、Fast DDS/Fast CDR 和源码 revision；准备全新的 work/prefix/evidence 路径。
3. **隔离构建域**：host generator 与 target prefix 分开；清除宿主 ROS/Conda/CMake package 污染；绝不让 `/opt/ros/humble` 满足 target 链接。
4. **执行 fresh cross build**：构建 adapter、安装 consumer，并从安装 prefix 再做 clean consumer；检查 AArch64、SONAME、NEEDED、RPATH/RUNPATH 和禁止依赖。
5. **完整部署**：通过 HDC 发送整个 target prefix 和所需 OHOS runtime，不要只发送 executable；设置 `LD_LIBRARY_PATH` 和目标网卡 Fast DDS profile。
6. **先验网络**：确认 host/target IP、route、carrier、网卡白名单；两端 `ROS_DOMAIN_ID`、topic、type 和 QoS 必须一致。
7. **双向验收**：先做 host ROS 2 ↔ OHOS，再按需做板内 ROS 2 CLI ↔ 最小应用；每个方向使用唯一 token/payload 并保存 stdout/stderr。
8. **生成证据再扩范围**：只有结构化 evidence 明确覆盖的范围才能写“通过”。单消息通过后，再进入多消息或全量产品 gate。

推荐流程中的环境相关值一律通过变量提供，不要把示例环境复制成固定配置：

```sh
: "${WORK_ROOT:?set WORK_ROOT to a fresh local work directory}"
: "${REMOTE_ROOT:?set REMOTE_ROOT to the target deployment directory}"
: "${HDC_TARGET:?set HDC_TARGET to the selected HDC target}"
: "${HOST_INTERFACE:?set HOST_INTERFACE}"
: "${HOST_IP:?set HOST_IP}"
: "${TARGET_INTERFACE:?set TARGET_INTERFACE}"
: "${TARGET_IP:?set TARGET_IP}"
: "${ROS_DOMAIN_ID:?set ROS_DOMAIN_ID}"

# 仅作为 host reference/generator 示例；不得让它参与 target 链接。
HOST_ROS_ROOT="${HOST_ROS_ROOT:-/opt/ros/humble}"
```

构建目录、安装 prefix 和 evidence 输出应派生自 `$WORK_ROOT`；所有 HDC 命令显式使用 `$HDC_TARGET`，所有目标端部署路径派生自 `$REMOTE_ROOT`；Fast DDS profile 使用 `$HOST_INTERFACE`/`$HOST_IP` 与 `$TARGET_INTERFACE`/`$TARGET_IP` 生成；两端显式设置同一个 `$ROS_DOMAIN_ID`。实际值及生成后的 profile 应写入本次运行 evidence。

完整命令、HDC 部署和排错步骤见 sibling 详细操作文档；执行时仍须用上述变量替换其中所有环境相关示例值：

> Sibling checkout：`IBMW_together/ros2_minimal_deps/docs/ohos-std-msgs-string-cross-build-deploy-interop.zh.md`

## 6. 验收门（缺一项就不能扩大结论）

### Gate A：构建

- 从空 work/prefix 成功构建，且 clean external consumer 成功；
- 所有目标 ELF 为 OHOS AArch64；无 host ELF、host 绝对路径或构建目录泄漏；
- 产品 NEEDED 闭包无 `rcl`、`rclcpp`、`rclpy`、`rmw_fastrtps_cpp`；基础 `librmw`、ROSIDL/typesupport、`rcutils` 的用途可解释；
- evidence 明确标注 build-only，不用“编译成功”替代板端执行。

### Gate B：部署与板端启动

- HDC 显示目标 `Connected`，设备选择器明确；
- prefix、补充 runtime 和 profile 均已部署，remote hash/权限/动态库闭包可核对；
- 程序在板端实际启动，网卡、IP、route、domain 和 profile 与记录一致。

### Gate C：wire 互通

- 标准 ROS 2 peer 与 OHOS 在两个方向都收到预期 typed payload；
- topic/type/QoS/CDR/DDS 版本和两个不同 endpoint 身份被记录；
- `ROS_DOMAIN_ID` 与部署路径按用户本次实际 evidence 填写，不能沿用历史快照或其他运行的值；
- 没有 pcap 就只声明应用层 typed interop，不声明 packet decode。

### Gate D：全量产品

- catalog/profile/source lock 完整，27 包/530 类型的生成、target 构建、回调/边界、消费者和 wire 证据分别可追溯；
- 每个 bundle 的依赖闭包、许可证、CMake exports、ABI/provider 和制品 manifest 通过；
- `.srv/.action` 子消息 serialization 不能冒充完整 service/action wire interoperability；
- 全量结论必须引用用户生成的全量 evidence，不能用本页单消息历史验证快照代替。

## 7. 文档出入清单

阅读旧文档时按以下规则消歧：

- [`IBMW docs/guides/cross-compilation.md`](cross-compilation.md) 明确是 **compile-only** 指南；其结尾也说明部署、发现、权限和板端运行需另验，不能据此声称实机互通。
- Sibling 旧 `ros2_minimal_deps/docs/PROGRESS.md` 与
  `ros2_minimal_deps/docs/architecture.md` 中部分时间点状态/“尚未完成”措辞已滞后。
  它们仍可解释历史和设计，但当前范围应以用户本次 evidence、
  `GOALS_AND_PROGRESS.md` 及本文验证摘要交叉核对。
- [`user-guide.zh.md`](user-guide.zh.md) 中 service 等章节的“100%（线协议）兼容”措辞过宽。没有对应 release/profile、双向 typed evidence、request/reply correlation 和完整协议 gate 时，不应外推为所有 ROS 2 类型、服务、动作、DDS vendor 或版本均兼容。
- 早期资料缺少 **HDC 部署链路**，因此只能覆盖构建，不能证明目标板装载与运行。当前单消息实操必须包含目标选择、完整 prefix 传输、权限、动态库、profile、remote path 和清理记录。
- 本文是范围与证据口径的 canonical 中文入口；详细命令以 sibling 操作文档为准，设计/全量产品目标以 sibling `GOALS_AND_PROGRESS.md` 为准，最终声明以用户为实际运行生成的结构化 evidence 为准。

## 8. 参考文档

### 本地文档

- [IBMW 交叉编译指南](cross-compilation.md)
- [IBMW 中文用户指南](user-guide.zh.md)
- [IBMW 支持矩阵](../reference/support-matrix.md)
- Sibling 详细操作指南：`IBMW_together/ros2_minimal_deps/docs/ohos-std-msgs-string-cross-build-deploy-interop.zh.md`
- Sibling 目标与进展：`IBMW_together/ros2_minimal_deps/docs/GOALS_AND_PROGRESS.md`
- Sibling 架构边界：`IBMW_together/ros2_minimal_deps/docs/architecture.md`
- Sibling 交叉编译契约：`IBMW_together/ros2_minimal_deps/docs/cross-compilation.md`
- Sibling DDS compatibility CLI：`IBMW_together/ros2_minimal_deps/docs/dds-compatibility-cli.zh.md`

### 官方文档

- ROS 2 Humble：Environment：<https://docs.ros.org/en/humble/Tutorials/Beginner-CLI-Tools/Configuring-ROS2-Environment.html>
- ROS 2 Humble：Topics：<https://docs.ros.org/en/humble/Tutorials/Beginner-CLI-Tools/Understanding-ROS2-Topics/Understanding-ROS2-Topics.html>
- ROS 2 Humble：Domain ID：<https://docs.ros.org/en/humble/Concepts/Intermediate/About-Domain-ID.html>
- ROS 2 Humble：Multiple RMW implementations：<https://docs.ros.org/en/humble/How-To-Guides/Working-with-multiple-RMW-implementations.html>
- eProsima Fast DDS 2.6 XML profiles：<https://fast-dds.docs.eprosima.com/en/v2.6.10/fastdds/xml_configuration/xml_configuration.html>
- eProsima Fast DDS 2.6 UDP/interface whitelist：<https://fast-dds.docs.eprosima.com/en/v2.6.10/fastdds/transport/udp/udp.html>
- OpenHarmony HDC 使用指导：<https://gitee.com/openharmony/docs/blob/master/zh-cn/device-dev/subsystems/subsys-toolchain-hdc-guide.md>
