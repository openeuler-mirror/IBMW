# OHOS DDS 与 ROS 2 最小依赖：概念与边界索引

> **“minimal ROS2 编译部署”的第一入口（含可执行的端到端命令）：**
> `IBMW_together/ros2_minimal_deps/docs/ohos-std-msgs-string-cross-build-deploy-interop.zh.md`
>
> **前置门：**入口要求认证的 `TARGET_RUNTIME_PREFIX` + `TARGET_RUNTIME_EVIDENCE` 与
> `CORE_SOURCE_CAPSULE` + `CORE_SOURCE_EVIDENCE`。本仓库组合当前不提供从互联网一键
> 生成完整底层 runtime 的命令；缺失
> 任一输入必须停止并交由 release 输入链准备，不能称为“从零构建”。producer 与里程碑索引
> 以 sibling runbook 第 1 节为准。

本文仅是 IBMW 侧的**概念、范围和证据边界索引**，不包含或替代端到端构建、打包、HDC 部署与互通命令。需要实际操作时先读上述 sibling 文档，再用本文核对 claim 边界；不要从零散进度文档或历史 evidence 推断当前能力。

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

快速 `std_msgs/msg/String` tier 还有以下不可省略的构建边界：

- core builder 必须在本次运行中重建 `builtin_interfaces` 与 `std_msgs`；
- 构建 IBMW 本体必须运行 `ibmw_dds_compat.py run --dds-only`，避免让 IBMW 的全量 `ros2_types` 误用单 String prefix；
- String adapter 与 String app/consumer 在 IBMW 本体之后，由单个 builder 的两个隔离
  CMake 阶段构建；不能把它们与 IBMW 当作一个隐式构建步骤；
- 若启用 IBMW 全量 `ros2_types`，prefix 必须包含 `geometry_msgs` 等更宽消息闭包；只含 `builtin_interfaces` + `std_msgs` 的单 String prefix 不满足该范围；
- minimal String packager `package_target_ohos_std_msgs_string.py` 与 27 package / 530 type 全产品 packager `package_target_ohos_sdk.py` 属于不同产品范围，输入、manifest 和结论均不得混用。

Sibling 仓库中的 `IBMW_together/ros2_minimal_deps/docs/GOALS_AND_PROGRESS.md`
记录了产品 catalog 的 **27 个 interface package / 530 个生成消息类型**。这是全量
产品工作的独立范围与进展，**不能说成已由本文的 `std_msgs/String` 历史验证快照
覆盖**；该快照只覆盖 fresh build-only 和单消息实机互通。

## 4. 验证状态与历史 evidence 边界

本文不宣称已完成本轮物理验证。仓库中的历史 validation snapshot 只用于解释边界，不是
用户当前 SDK、源码、制品、设备或网络的 evidence；本轮各项在产生并审核当前原始证据前
均为 `NOT VERIFIED`。

| 项目 | 本轮状态 | 通过所需证据 | 未证明或限制 |
| --- | --- | --- | --- |
| fresh OHOS cross build | `NOT VERIFIED` | 本轮 fresh prefix、builder evidence、consumer 与 ELF/closure 审计 | build-only 不覆盖 target 执行或 G3/G4 |
| 物理双向互通 | `NOT VERIFIED` | 本轮 Connected target、网络/profile、两个方向唯一 payload 与双方退出码/日志 | 即使通过也只覆盖本轮单消息、版本组合和应用层结果 |
| generic G3 / generic G4 | 未完成 | 无 | 不得据上述两项推断完成 |
| pcap / packet decode | 未完成 | 无 | 只能声明应用层 typed interop，不声明报文级解码通过 |

用户必须为自己的每次运行生成 evidence，且至少满足以下规则：

- evidence 由实际命令和输出生成并保存在用户自己的受控工作区；本文不把未入库文件、口头记录或历史快照当作当前运行的证明；
- 记录 SDK/toolchain、源码 revision、消息定义、DDS/CDR 与 ROS 2 profile、构建参数、制品 manifest/hash、目标架构和依赖审计；
- 互通运行还要记录实际使用的 host/target 接口与地址、`ROS_DOMAIN_ID`、topic/type/QoS、两端身份、唯一 payload、stdout/stderr，以及是否具备 packet capture/decode；
- build-only、板端启动、单向通信、双向通信和 packet decode 必须分别标注，未执行项写明未验证；
- SDK、源码 revision、消息定义、DDS/CDR 版本、toolchain、制品或网络配置变化后，必须重新生成 evidence，不能沿用历史结论。

## 5. 推荐阅读与执行顺序

1. **进入操作文档**：先打开 `IBMW_together/ros2_minimal_deps/docs/ohos-std-msgs-string-cross-build-deploy-interop.zh.md`，以其中当前命令和脚本 `--help` 为准。
2. **先选层级**：第一次只做 `std_msgs/String`，不要直接把全量 530 类型、service/action 或 generic G3/G4 当作起点。
3. **确认设备**：任何部署前都必须读取本次 `hdc list targets -v` 输出，并确认所选目标状态为 `Connected`；未连接时停止，不能用历史设备记录代替。
4. **固定并隔离输入**：记录 SDK、toolchain、profile、依赖和源码 revision，使用全新的 work/prefix/evidence；host generator 与 target prefix 分离。
5. **按 tier 构建**：重建 `builtin_interfaces` + `std_msgs` core；以 `ibmw_dds_compat.py run --dds-only` 构建 IBMW；再由单个 builder 以两个隔离 CMake 阶段构建 String adapter 和 app/consumer。
6. **审计并打包**：检查 AArch64、SONAME、NEEDED、RPATH/RUNPATH 和禁止依赖；只使用 minimal String packager，禁止混入 27/530 全产品 packager 的输入或产物。
7. **完整部署与网络预检**：部署整个目标闭包和 profile，不只传 executable；核对当前 host/target 网络、domain、topic、type 与 QoS。
8. **当前物理验收**：在本次已连接设备与物理网络上执行双向 typed payload 验收并保存原始输出；历史 validation snapshot 只能提供背景，不能替代本次 build、deploy、load、run 或 interop 验证。
9. **按 evidence 扩范围**：只有本次结构化 evidence 明确覆盖的范围才能写“通过”；单 String 结果不得外推到全量产品。

环境相关值必须由本次运行提供并写入 evidence，不得把文档示例、历史设备、固定网络或部署路径复制为当前配置。完整命令、HDC 部署和排错步骤只在 sibling 详细操作文档维护：

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
- 本文是 IBMW 侧的范围与证据边界索引；minimal ROS2 编译部署的第一入口及详细命令以 sibling 操作文档为准，设计/全量产品目标以 sibling `GOALS_AND_PROGRESS.md` 为准，最终声明以用户为实际运行生成的结构化 evidence 为准。

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
