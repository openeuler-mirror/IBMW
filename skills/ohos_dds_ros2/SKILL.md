# OHOS + DDS/ROS2 静态消息交付

用于代理或工程师在 OHOS/AArch64 上交付 DDS wire-compatible、ROS2
最小目标依赖和静态消息流程。这里的“ROS2 兼容”指 DDS 线协议互操作，不表示
目标程序必须携带 ROS2 client library。

## 触发条件

出现以下任一需求时使用本技能：

- OHOS 板端与 ROS2/DDS peer 发布或订阅同一静态消息；
- Route B、DDS compatibility、ROS2 minimal dependency 或 target closure 裁剪；
- 交叉构建、HDC 部署、板端 CLI、抓包或互操作证据；
- 需要判断某项结果究竟只证明 build、deploy、load、run、interop 还是 packet。

不适用于仅有 JSON/local 后端、动态消息定义、通用 Action 迁移，或仅做主机
ROS2 应用开发的任务。

## Canonical 文档

开发者询问“minimal ROS2 编译部署”时，**第一入口必须是**：

- `IBMW_together/ros2_minimal_deps/docs/ohos-std-msgs-string-cross-build-deploy-interop.zh.md`（端到端命令）

该入口的硬前置是认证的 `TARGET_RUNTIME_PREFIX` + `TARGET_RUNTIME_EVIDENCE` 与
`CORE_SOURCE_CAPSULE` + `CORE_SOURCE_EVIDENCE`。
仓库当前没有从互联网一键生产完整底层 runtime 的单命令；缺失时必须停止并交给 release
输入链准备，禁止称为“从零构建”。producer 与里程碑只按 canonical runbook 第 1 节索引，
不得复制环境私有参数。

随后再读 IBMW 的概念/边界索引及相关参考，并以仓库当前版本为准：

- [OHOS DDS / ROS2 最小依赖概念与边界索引](../../docs/guides/ohos-dds-ros2-minimal-interop.zh.md)
- [OHOS ARM64 + ROS2 Humble 交叉构建指南](../../docs/guides/ohos-xiaoyi-humble-cross-build-guide.zh.md)
- [交叉编译指南](../../docs/guides/cross-compilation.md)
- [ROS2 interface import 指南](../../docs/guides/ros2-interface-import-user-guide.md)
- [支持矩阵](../../docs/reference/support-matrix.md)
- [Route B/OHOS cross gate](../migration/tasks/08-route-b-ohos-cross.md)
- [Route B/OHOS checklist](../migration/checklists/route_b_ohos.md)
- [Route B/OHOS SDK 规则](../sdk/tasks/03-route-b-ohos-sdk.md)

若命令示例与 canonical 文档或当前脚本 `--help` 冲突，停止并以当前文档/脚本
为准；不要凭旧 evidence 反推参数。

## 0. 前置检查

开始前记录，而不是猜测：

1. 源码 revision、干净且唯一的 build/install/evidence 目录；复用 cache 时核对
   `CMAKE_HOME_DIRECTORY` 与 `CMAKE_GENERATOR`，不得切换生成器。
2. OHOS Native SDK 路径、版本、sysroot、toolchain、target triple 与预期
   `aarch64` 架构。
3. `hdc version`（或设备所支持的等价版本命令）、`hdc list targets` 输出，
   选定且唯一的 target serial；后续命令显式绑定该 target。
4. target 型号/系统 build、ABI、可写部署目录、可执行权限及可用空间。
5. host、容器、target underlay/overlay、SDK sysroot、第三方 provider 的边界；
   记录容器镜像及 mount（如适用）。
6. 网络接口、host/target IP、路由、端口/组播策略、防火墙、同网段性；记录
   Fast DDS profile/XML 的实际路径、哈希和是否成功加载。
7. 两端 `ROS_DOMAIN_ID`/DDS domain、消息 type name/type hash（若可用）、topic
   name、reliability、durability、history/depth、deadline/liveliness 等 QoS；两端
   必须兼容，不能只比较 topic 名。
8. 静态消息 IDL/`.msg` 的唯一来源、生成器版本、生成输出和最大序列化尺寸预算。
9. FastDDS/FastRTPS、Fast-CDR、SDK libc++、tinyxml2 的 provider、realpath、
   SONAME、架构和许可证/部署许可。

任一关键输入未知，先标 `NOT VERIFIED`，不得用默认值制造 PASS。

## 1. 选择 scope（只选一个）

| Scope | 目标 | 最低交付边界 |
|---|---|---|
| `substrate` | 证明 OHOS DDS substrate 可构建、装载和运行 | 一个 target DDS smoke；不声称 ROS2 消息互操作 |
| `single-message` | 证明一个固定静态消息与隔离 ROS2/DDS peer 双向互操作 | 一个明确 type/topic，host→board 与 board→host，含 payload 校验 |
| `full-static-product` | 交付产品声明内全部静态消息流程 | 消息清单逐项映射 producer/consumer、QoS、容量、双向/单向要求和六级证据 |

默认从 `single-message` 开始。`substrate` 不能升级推断消息兼容；一个消息的
PASS 不能代表产品全集。`full-static-product` 中未执行的矩阵项必须明确列为
`NOT VERIFIED`。

## 2. 目标 closure 与 provider 规则

- host `/opt/ros`、host ROS2/FastDDS 库、头文件路径和 Python extension **禁止**
  进入 target 编译、链接、安装配置或运行 closure。配置时清理
  `CMAKE_PREFIX_PATH`、`COLCON_PREFIX_PATH`、`ROS_PACKAGE_PATH`，只显式加入
  OHOS target prefix/overlay 和必要的 host-only generator 路径。
- 允许将 `/opt/ros/<distro>` 中的 ROS2 CLI/node 作为**隔离的 reference peer**
  运行在 host/容器；必须标注 `(host reference peer)`，其进程和库不得复制到
  target，也不得作为 target provider。
- target 基础 `librmw` 可以存在；负向审计禁止的是目标应用 closure 中的
  `rcl`/`rclcpp`/`rclpy`/`rmw_fastrtps_cpp`，不是对所有 `librmw` 文件做一刀切
  禁止。若应用实际 `DT_NEEDED`/运行加载了基础 `librmw`，记录用途和 provider。
- 优先使用选定 OHOS target ROS2 prefix 提供的匹配 FastDDS/Fast-CDR；不得静默
  混用 bundled、`_deps` 或 host 版本。核对 FastRTPS 对 Fast-CDR 的
  `DT_NEEDED` 与所选 `libfastcdr` SONAME 完全匹配。
- SDK provider 的 target `libc++`（以及实际需要的 `libc++abi`/unwind）必须按
  SDK 许可和 loader 规则部署；`tinyxml2` 若被 profile/XML 路径实际依赖，也要
  部署 target 架构的 provider。不得用 host 副本补缺。记录每个文件的来源、
  hash、SONAME、目标路径，并用 loader 输出证明最终解析位置。

## 3. 执行流程

快速 `std_msgs/String` tier 必须严格按以下顺序执行：

1. 建立本次 case 和全新 work/prefix/evidence，记录当前输入；
2. 用 core builder 重建 `builtin_interfaces` + `std_msgs`；
3. 构建 IBMW 本体时执行 `ibmw_dds_compat.py run --dds-only`；
4. 在 IBMW 之后，由单个 builder 的两个隔离 CMake 阶段构建 String adapter 与 String
   app/consumer；
5. 审计后仅用 minimal packager `package_target_ohos_std_msgs_string.py` 打包；
6. 部署前运行 HDC 目标查询并确认所选目标为 `Connected`；
7. 部署、load、run，并在当前物理设备/网络上尝试双向 interop；只有本轮 evidence 通过后
   才能写“完成”，文档本身不宣称本轮物理验证已完成。

不得跳过 `--dds-only`：IBMW 全量 `ros2_types` 需要含 `geometry_msgs` 等依赖的更宽
prefix，单 String prefix 不满足它。也不得将 minimal packager 与 27 package / 530 type
全产品 packager `package_target_ohos_sdk.py` 的输入、manifest、产物或 claim 混用。
历史 evidence 只可说明背景，禁止替代上述当前构建和当前物理验证。

### A. Build

按 sibling 端到端操作文档使用显式 SDK、target prefix、host generator 与全新 build
目录，并遵守上述 core → IBMW `--dds-only` → adapter → app/consumer 顺序。保存完整
命令、环境白名单、configure/build log、生成消息清单和
`route_b_target_artifact_audit.json`。安装到干净 prefix，并用干净 external consumer
验证 `find_package(ibmw CONFIG REQUIRED)`。

对每个将部署的 target ELF（可执行文件及递归动态库 closure）执行 `file`、
`readelf -h -d`、SONAME/RPATH/RUNPATH/`DT_NEEDED` 审计，并扫描字符串、动态符号
和解析后的 closure：

```text
禁止命中：host /opt/ros 路径；rcl；rclcpp；rclpy；rmw_fastrtps_cpp
允许但须解释：基础 librmw
```

不能只扫描文件名；记录命中对象、字段和解析后的 provider。绝对 `DT_NEEDED`、
host/build-tree 路径、非 AArch64 ELF 或未解析依赖均为失败。

### B. Deploy

先用本次 HDC 查询确认显式 target serial 状态为 `Connected`；否则停止并标记
`NOT VERIFIED`。再创建本次 case 目录，部署应用、配置、
消息资源及递归 closure，包括所需 SDK `libc++` 和 target `tinyxml2`。保存本地与
远端 manifest（路径、大小、SHA-256），再逐项比对；不要把“传输成功”写成
“可加载”。

### C. Load

在干净 target shell 中设置最小 loader/profile 环境，执行 loader trace 或等价
诊断。证明所有库从预期 target 目录解析、无缺失符号、profile 确实被读取；记录
退出码和 stderr。装载成功不证明 main loop 或 DDS 数据面工作。

### D. Run

启动 target 进程，记录 pid、命令、环境、domain、network/profile、QoS、开始/
结束时间、退出码和应用 ready/receive/send 标记。执行本地 target smoke 与最大
代表 payload；“进程仍在”不等于业务 PASS。

### E. Interop（双向）

必须完成两条独立验收：

1. **主机 CLI/reference peer → 板内 CLI/应用 harness**：host 发布带唯一
   case/sequence 的 payload，板内 CLI/日志验证完整字段和值。
2. **板内 CLI/应用 harness → 主机 CLI/reference peer**：板端发布另一唯一
   payload，host ROS2/DDS CLI 订阅并验证完整字段和值。

每条都记录两端命令、type/topic、domain、profile、network、QoS、时间窗、发送
与接收计数及 payload。仅 discovery、topic list、endpoint matched 或单向成功
不能证明双向互操作。若产品设计本来单向，另一方向作为验收探针执行；确实无法
执行时必须标 `NOT VERIFIED`，不得改写为双向 PASS。

### F. Packet proof

在已授权接口抓取与该 case 对应的数据包，保存 pcap、抓包命令/接口/过滤器、
时间同步信息和 SHA-256。证明 participant discovery 与目标 topic 的 user-data
流量可关联到唯一 case/sequence；加密流量只能证明关联流量存在，不能凭包内容
声称 payload 正确。packet proof 不能替代应用层 interop payload 验证。

## 4. 六级 claim ladder（不可互推）

`build`、`deploy`、`load`、`run`、`interop`、`packet proof` 是六个独立 gate。
任一级 PASS 不自动证明前一级或后一级，也不能用较高层结果补写缺失证据。报告
逐级标记 `PASS`、`FAIL` 或 `NOT VERIFIED`，每级只引用本级原始证据：

- build：交叉构建、安装 consumer、ELF/closure audit；
- deploy：manifest 与 host/target hash 一致；
- load：loader 解析与符号结果；
- run：target 进程及业务 smoke；
- interop：主机和板内 CLI/reference peer 双向 payload 验收；
- packet proof：pcap 及 case 关联分析。

## 5. 证据读取字段

读取 JSON、日志、manifest 或 pcap sidecar 时至少提取并在报告中原样引用：

- provenance：case ID、scope、时间、操作者、源码 revision、命令、cwd、容器、
  工具版本、证据路径与 SHA-256；
- build/audit：`status`、`expected_arch`、每个 `artifacts[].path`/`arch`，ELF
  `DT_NEEDED`、SONAME、RPATH/RUNPATH、provider realpath，以及 forbidden-hit
  结果；字段缺失不能视作 pass；
- device：HDC version、target serial、系统 build、ABI、部署根目录；
- DDS：domain、participant、topic、type、profile 路径/hash/load 结果、network
  interface/IP、QoS 全字段；
- runtime：pid、start/end timestamp、exit code、send/receive count、case ID、
  sequence、payload expected/actual；
- packet：capture interface、filter、start/end、packet count、case/sequence
  关联方法、pcap 路径/hash。

证据只证明它实际包含的字段。旧路径、编号或口头描述不是 provenance；不得把任何
历史部署目录或 domain 当作当前运行的正式 evidence。只有本次读取、校验并纳入
manifest 的实际路径、设备和文件才能引用。

## 6. 输出报告模板

```markdown
# OHOS DDS/ROS2 static-message report
- Case ID / scope:
- Source revision / time / operator:
- HDC version / target serial / OHOS build / ABI:
- SDK/sysroot / target underlay+overlay / DDS providers:
- Message inventory (type, topic, direction, max size):
- Domain / network / profile path+hash+loaded / QoS:
- Deploy root (本次实际路径；不得预填历史路径):

## Closure audit
- ELF arch / interpreter / DT_NEEDED / SONAME / RPATH/RUNPATH:
- Provider realpaths: FastDDS, Fast-CDR, libc++, tinyxml2, optional librmw:
- Forbidden hits (/opt/ros, rcl, rclcpp, rclpy, rmw_fastrtps_cpp):
- Audit JSON path / status / expected_arch / artifacts / SHA-256:

## Claim ladder
| Gate | PASS/FAIL/NOT VERIFIED | Raw evidence | Key fields |
|---|---|---|---|
| build | | | |
| deploy | | | |
| load | | | |
| run | | | |
| interop host CLI→board CLI/harness | | | |
| interop board CLI/harness→host CLI | | | |
| packet proof | | | |

## Payload and packet results
- Expected/actual case ID, sequence, fields, counts:
- pcap path/hash/interface/filter/correlation:

## Exceptions, failures, and unverified items
- ...

Final claim: <严格限定到 scope 与已 PASS gate>
```

## 7. 停止条件

命中任一项立即停止升级 claim，保留日志并报告 `FAIL` 或 `NOT VERIFIED`：

- SDK、target prefix、target serial、消息 schema、domain/profile/QoS 或 provider
  来源不明确；HDC 设备歧义或版本/目标与记录不符；
- host `/opt/ros` 进入 target closure，或 target ELF/解析 closure 命中
  `rcl`、`rclcpp`、`rclpy`、`rmw_fastrtps_cpp`；
- ELF 非 AArch64、绝对 `DT_NEEDED`、未解析库、SONAME/ABI 冲突，或混入 host/
  build-tree provider；
- SDK `libc++` 或所需 target `tinyxml2` 未部署、hash 不一致、loader 从意外路径
  解析，或 profile 未加载；
- 网络、domain、QoS 不兼容，最大静态 payload 超预算，进程异常退出或 payload
  字段/计数不匹配；
- 只有 discovery、单向日志、旧 evidence、无 hash 文件，或 pcap 无法关联本次
  case；
- 试图从六级中的任一级推断另一等级，或从 `single-message` 外推
  `full-static-product`。

失败后不得删除或覆盖原始 evidence；修复输入后使用新的 case ID、干净 build
目录和独立报告重跑。
