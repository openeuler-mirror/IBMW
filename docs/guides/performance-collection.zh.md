<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# ROS 2 与 IBMW 性能采集指南

## 1. 适用对象

本指南适用于需要在 Linux 单机上采集以下证据的开发者和性能工程师：

- ROS 2 业务或受控 benchmark 的环境、Graph、进程资源、网络与逐消息 latency artifact；
- IBMW 业务的外部环境、构建/Transport 信息、进程资源与 Core Metrics artifact；
- 已分别完成 A、B、C 运行后，对结果目录做一致性检查和最小聚合。

工具不会替你选择公平场景，也不会自动把非等价 Transport 变成 DDS 等价方案。正式结论应先阅读[设计基线](../architecture/performance-observability-design.zh.md)。

## 2. 前置条件：构建、安装与关闭

- 先按被测场景构建 IBMW；需要 Core Metrics 时必须使用
  `-DIBMW_ENABLE_CORE_METRICS=ON`，需要内部 `comm_trace` 时还必须使用
  `-DIBMW_ENABLE_COMM_TRACE=ON`。`comm_trace` 是内部 trace skeleton，不是
  公共应用 API，业务代码不得依赖它。
- 若从安装布局运行 collector 或被测程序，必须先完成
  `cmake --install <build-dir> --prefix <install-prefix>`，并从同一安装前缀
  解析运行时库、扩展和工具；不能把 build-tree 私有头文件当作安装契约。
- managed 采集要求 SUT 能响应正常的 Engine shutdown。Core Metrics、producer
  latency 和最终 artifact 依赖优雅关闭；`SIGKILL`、超时强杀或 attached 模式
  不能产生 required 的最终证据。attached 只采集已有进程，不负责启动或停止。

本指南区分四种证据等级：**functional**（功能成功或失败）、**descriptive**
（环境、构建、资源和观测摘要）、**formal**（满足已版本化合同、完整性和
provenance 校验的指标/比较证据）、**product claim**（面向产品的性能、能力或
兼容性承诺）。前一级不能自动升级为后一级；没有合同、样本不完整、来源不明
或校验失败时必须 `unsupported` 或 fail-closed，不能用 0、推测或局部数字补齐。

## 3. 当前已实现能力

| 能力 | 当前状态 |
|---|---|
| `doctor/run/validate/report` | 已实现 |
| 单机 managed/attached | 已实现 |
| CPU、PSS/RSS、线程、network counters | 已实现 |
| ROS 环境、Graph、topic readiness | 已实现，被动 CLI 采集 |
| ROS producer-owned latency JSONL | 已实现，producer 必须支持合同 |
| IBMW External P0 与 provenance | 已实现 |
| IBMW Core Metrics JSONL 归档/验证 | 已实现 |
| `compare` | 已实现 MVP，只聚合并重校验已有 run |
| 独立 A/B/C runner | 已有固定 4 KiB benchmark 原型 |
| generic ROS Topic Observer、trace/pcap、两机 Agent | 未提供 |

以下命令均从 IBMW 仓库根目录执行，使用最终布局：

```bash
COLLECTOR=./tools/mw_perf_collect/mw-perf-collect
```

## 4. 前置条件

- Linux，且可读取目标进程的 `/proc`；PSS 需要读取 `/proc/<pid>/smaps_rollup`；
- Python 3；JSON 配置不依赖第三方包，YAML 配置需要 PyYAML；
- ROS2 Adapter 可使用已 source 的环境；安装后的 clean collector 建议配置 canonical、非符号链接的 `adapters.ros2.ros2_prefix`，由工具仅从该目录派生 ROS CLI 的 Python 和共享库路径；
- IBMW Adapter 需要可读的仓库、构建目录和被测二进制；
- Core Metrics 需要被测 IBMW 构建已包含该能力，且 SUT 能正常走 Engine shutdown；
- 容器内采集时需确认 `/proc` 和 `/sys/class/net` 是目标 namespace 的视图。当前不存在的网络接口会被静默跳过，不构成自动 gate；运行后必须检查 `raw/network.jsonl` 中目标接口确实存在。

工具不执行交互式 sudo。权限不足的指标会降级或使 required gate 失败。

配置 `ros2_prefix` 时，工具要求 `<prefix>/bin/ros2` 是 prefix 内 canonical executable，并从其无参数 absolute Python shebang 验证 interpreter identity 和 Python 版本；随后仅从该 prefix 派生 `ros2cli` package path 与共享库路径。Graph 子命令使用 `--no-daemon` 直接发现，不依赖或复用后台 ROS 2 daemon。调用方或 SUT 的 `PYTHONPATH`、`PYTHONHOME`、`LD_PRELOAD`、`LD_LIBRARY_PATH` 不会进入该可信 CLI 环境；prefix、shebang 或包闭包不满足条件时 doctor 降级/失败，不应退回污染环境补齐。

## 5. managed 与 attached

### managed

工具负责启动、采样和停止 SUT。命令优先写成数组，避免 shell quoting：

```json
{
  "sut": {
    "mode": "managed",
    "command": ["./build/my_sut", "./build/cfg/my_sut.yaml"],
    "working_directory": ".",
    "env": {}
  }
}
```

managed 会跟踪 leader 与后代进程。若配置中已有 `sut.command`，又在 CLI 的 `--` 后传命令，必须加 `--override-command`。

### attached

工具只附着已有 PID，不启动也不停止它：

```json
{
  "sut": {
    "mode": "attached",
    "pid": 12345
  }
}
```

attached 会校验 PID start ticks，目标在采样期退出则运行无效。由于 collector 不控制目标最终关闭，required producer latency/Core Metrics 不适用于 attached；optional artifact 只能作为诊断数据。

## 6. 完整 JSON 配置

下面是一份包含当前公共字段、ROS2 Adapter 和 IBMW Adapter 的完整参考。单次运行只选择一个 `--adapter`，未选 Adapter 的配置不会执行。路径相对于仓库根目录；按你的构建产物修改。

```json
{
  "schema_version": 1,
  "run": {
    "warmup_seconds": 2,
    "sample_seconds": 10,
    "stop_timeout_seconds": 5
  },
  "sut": {
    "mode": "managed",
    "command": ["./build/my_sut", "./build/cfg/my_sut.yaml"],
    "working_directory": ".",
    "env": {
      "ROS_DOMAIN_ID": "42"
    }
  },
  "collectors": {
    "process": {
      "interval_ms": 1000,
      "include_descendants": true
    },
    "network": {
      "interfaces": ["lo", "eth0"]
    }
  },
  "scenario_contract": {
    "id": "single-host-cross-process-4k-v1",
    "measurement_lane": "normalized_benchmark",
    "deployment": "single_host_cross_process_1to1",
    "topic": "/mw_perf/payload_4kb",
    "type": "std_msgs/msg/UInt8MultiArray",
    "payload_bytes": 4096,
    "reliability": "reliable",
    "durability": "volatile",
    "history": "keep_last",
    "depth": 8192,
    "offered_rate_hz": 1000,
    "warmup_messages": 50,
    "measured_messages": 3000,
    "completion": "3000 unique valid measured messages",
    "resource_accounting_profile": "sut_and_required_middleware_v1"
  },
  "adapters": {
    "ros2": {
      "ros2_prefix": "/opt/ros/humble",
      "readiness_timeout_seconds": 10.0,
      "readiness_poll_interval_seconds": 0.2,
      "topics": [
        {"name": "/mw_perf/payload_4kb", "count": 1}
      ],
      "latency_artifact": {
        "path": "/tmp/ibmw-perf-collection/ros2-latency.jsonl",
        "required": true
      }
    },
    "ibmw": {
      "root": ".",
      "build_dir": "./build",
      "transport_config": "./build/cfg/my_sut.yaml",
      "core_capability": "./build/core-metrics-capability.json",
      "core_metrics": {
        "directory": "/tmp/ibmw-perf-collection/ibmw-core",
        "required": true
      },
      "provenance": {
        "required": true,
        "sut_executable": "./build/my_sut",
        "artifact_binaries": [
          "./build/my_sut",
          "./build/libibmw_dds_transport.so"
        ],
        "runtime_images": [
          {
            "role": "sut",
            "name": "my_sut",
            "source": "exe",
            "expected_path": "./build/my_sut"
          },
          {
            "role": "transport",
            "name": "dds_transport",
            "source": "maps",
            "expected_path": "./build/libibmw_dds_transport.so"
          }
        ]
      }
    }
  }
}
```

注意：

- required IBMW provenance 必须有非空 `scenario_contract`；
- `provenance.required=true` 时还必须提供非空 `provenance.runtime_images` declarations；缺失会在 run preflight 失败，`validate/compare` 无条件要求完整 runtime image evidence；
- `source: "maps"` 只接受被采样进程 `/proc/<pid>/maps` 中 permissions 格式合法且含 `x` 的实际可执行映射；PID/start ticks、device/inode、路径、大小或 SHA-256 任一不一致均 fail-closed，只读 data mmap 不算匹配；
- `core_capability` 是可选的能力描述文件，省略时 External P0 仍可工作；
- `core_metrics.directory` 中工具读取 `metrics.jsonl`；
- `latency_artifact.path` 必须由本轮 producer 写入，旧文件不算本轮证据；
- JSON Schema 允许扩展字段，但工具只执行当前实现读取的字段。

## 7. ROS2 使用流程

### 7.1 doctor

```bash
source /opt/ros/humble/setup.bash
$COLLECTOR doctor --adapter ros2 --config ros2-run.json
```

`doctor` 不启动 SUT。重点检查 `status`、ROS 环境、ROS 2 CLI 可达性、RMW 以及公共 `/proc`、sysfs network 和 YAML 能力。ROS 发行版从 `ROS_DISTRO` 记录；工具不调用 Humble 不支持的 `ros2 --version`。

### 7.2 run

```bash
$COLLECTOR run --adapter ros2 --config ros2-run.json --output runs/ros2
```

如果命令不写入配置：

```bash
$COLLECTOR run --adapter ros2 --config ros2-run.json \
  --output runs/ros2 --override-command -- ./build/my_ros2_sut
```

ROS Adapter 只做 Graph/readiness 和 artifact 校验。逐消息 latency 不是 `ros2 topic hz` 推导值；producer 必须输出 `mw_perf.latency.v1` JSONL。

### 7.3 validate

```bash
$COLLECTOR validate runs/ros2/<run-id>
```

检查 `valid_for_collection`、`valid_for_metric`、`valid_for_comparison` 和 `failure_codes`。required latency 的 completion、运行身份、hash 或优雅 finalization 任一失败，结果不可比较。

### 7.4 report

```bash
$COLLECTOR report runs/ros2/<run-id>
```

该命令从现有 artifact 重建 `validation.json` 和 `report.md`，不重跑 SUT。当前报告是单轮摘要，不是正式 A/B/C 统计报告。

## 8. IBMW External、Core Metrics 与 comm_trace

External metrics 是 collector 从 SUT 外部采集的环境、进程、网络、构建和
Transport/provenance 信息；它不需要 Core Metrics。Core Metrics 是 IBMW runtime
内部按 schema 输出的 `metrics.jsonl`，须由启用的构建和正常 Engine shutdown
完成归档。`comm_trace` 是更低层的内部通信 trace，用于受控诊断/候选验证；它
既不是 External metrics，也不是 Core Metrics 的公共替代品，更不是公共应用 API。
不要把 trace 事件数量当成性能指标或产品能力证明。

### 8.1 仅 External P0

不要求 Core Metrics 时，删除 `core_metrics` 或设为 optional，并执行：

```bash
$COLLECTOR doctor --adapter ibmw --config ibmw-external.json
$COLLECTOR run --adapter ibmw --config ibmw-external.json --output runs/ibmw
```

你仍会得到 source/build/Transport 摘要、进程资源、网络和 capability。Core Metrics 显示 `unsupported` 不代表 External P0 失败。

### 8.2 开启 Core Metrics

在 managed SUT 环境中设置：

```json
{
  "env": {
    "IBMW_CORE_METRICS": "counters",
    "IBMW_OBSERVABILITY_OUTPUT_DIR": "/tmp/ibmw-perf-collection/ibmw-core"
  }
}
```

并让 Adapter 指向同一目录：

```json
{
  "core_metrics": {
    "directory": "/tmp/ibmw-perf-collection/ibmw-core",
    "required": true
  }
}
```

SUT 必须正常走 Engine shutdown 才会生成 final `metrics.jsonl`。collector 在停止 SUT 后将变化后的文件归档为 `raw/core_metrics.jsonl`，校验 schema、递增 `snapshot_seq`、`kind`、`cumulative`、`complete`、hash 和 provenance。

关闭内部计数但保留外部采集：

```json
{
  "env": {"IBMW_CORE_METRICS": "off"}
}
```

Core Metrics 的 formal host 性能门已在既定 host capability/合同上通过，但 counters 仍不应在生产环境中默认打开，也不能用内部计数替代 A/B/C 公共外部指标或外推到 OHOS。

### 8.2.1 shm 传输的 Core Metrics 性能门禁

只构建自研 shm 传输（不构建 iceoryx）时，用 `scripts/core_metrics_shm_perf.py` 代替 `scripts/core_metrics_perf.py`。两者参数相同、判定代码相同，区别是协议 `core-metrics-shm-lossless-v1`、通道 `shm_loaned_ctx`、kind `direct_shm_loan`，不需要 RouDi。

```bash
source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH=/opt/ros/humble/lib
# 先做守恒诊断（约 5 分钟，含两次沙箱构建；不给验收结论）
python3 scripts/core_metrics_shm_perf.py --output /path/to/empty-diag --jobs 10 \
  --dependency-source-pool /path/to/deps-pool --diagnostic-conservation
# 完整验收（24 块 × 3 模式，每次 60 s，约 2 小时）
python3 scripts/core_metrics_shm_perf.py --output /path/to/empty-run --jobs 10 \
  --dependency-source-pool /path/to/deps-pool
```

- 输出目录必须是新的空目录，且不能位于仓库根或任一快照根（`cmake`、`src`、`scripts`、`tools`、`selfdev` 等）之下，所需空间约 300 MB（两个构建树与源码、依赖快照）。
- 依赖源码池按 `scripts/contracts/core-metrics-shm-dependencies-v1.json` 校验，内容不一致即停止。
- 绑定 CPU 25、26、27；每次试验使用私有 `/tmp/ishm*` 根并在结束后删除，不写 `/dev/shm`。
- 结果只在 shm 协议内比较 off/out/counters，不要与 iceoryx 门禁的数值对比（进程数与冻结负载不同）。

**干扰核算（shm 门禁）。** 本机常有他人负载。`scripts/core_metrics_shm_perf.py` 只判定运行本门禁进程的 CPU：E2E 为 26（订阅）和 27（发布），micro 为 27。CPU 25 和共享 L2 的 24 只作为上下文记录。

E2E 在门禁自己的 CPU 采样窗口内（不含预热与冷却）约每秒取一个原始样本。每 CPU 的 foreign = max(`/proc/schedstat` 运行时间 − 本门禁进程在该 CPU 上的 utime+stime, 逐任务扫描归因的外来时间)。样本按原始计数封存，加载时重新推导并校验首尾相接。不用 `/proc/stat` 作被减数，因为它的忙时是 tick 采样：本通道 5750 批/秒恰为每 tick 23 批，读数会在 0.06–2.04 核之间混叠。

判为受干扰的条件（阈值在验收前由本机噪声数据确定，并登记在计划中）：

- 两 CPU 窗口均值之和 > 0.02 核；
- 或任一 CPU 的 1 s 样本 > 0.10 核：无突发试验最大 0.039 核，最小的外来突发 0.12 核；
- 或出现 `accounting_deficit`、`sampler_gap`。

micro 用紧贴基准运行的专用窗口：foreign = CPU 27 的运行时间 − 基准 rusage（扣除登记的 1 ms 启动余量后判 deficit），再加 CPU 27 的 run delay。超过基准时长的 1% 即受干扰。

受干扰的试验 `quality_valid=false`，所在通道结论为 INCONCLUSIVE。通道之间仍按 FAIL > INCONCLUSIVE > PASS 合并。`summary.json` 给出 `e2e_reason`/`micro_reason`，只有总结论为 INCONCLUSIVE 时才有顶层 `reason: interference`。每次试验前要求 2 s 安静窗口，最长等待 10 min。

2026-11-05 的主机验收结果为 `FAIL (E2E INCONCLUSIVE, micro FAIL)`：E2E 72 次中 38 次受干扰；micro 90 次均 clean，其 FAIL 在 iceoryx 构建上同样出现（counters/off 10.86），来自每次 add 一次原子读改写（约 5 ns）对 0.024 ns 预算的超出，与传输和干扰无关。主机结果不外推到 OHOS，性能结论建议在 OHOS 设备上测得，并同样检查环境干扰。

### 8.3 OHOS remote capture candidate

OHOS 上 host collector 的 inherited FD 不能穿过 HDC。需要验证设备端 DDS exact capture 时，可分别启用默认关闭的 native agent 与 production candidate：

```bash
cmake -S . -B <ohos-build-dir> \
  -DIBMW_BUILD_OHOS_CAPTURE_AGENT=ON \
  -DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=ON \
  -DIBMW_ENABLE_COMM_TRACE=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_PROTOCOLS=ON
cmake --build <ohos-build-dir> --parallel
```

完整 agent 参数、安全约束见 [native capture README](../../tools/mw_perf_collect/native/README.md)，production candidate 的构建、安装组件和 `--exit-after-complete` 约束见 [DDS managed exact candidate](../../src/extensions/transport/dds_ext/DDS_MANAGED_EXACT_CANDIDATE.md)。将 agent、candidate、DDS extension 以及目标 ELF `NEEDED` 的 provider/platform shared objects 通过 HDC 部署到设备；candidate 保持 `bin`、extension 保持 `lib` 布局。设备运行目录必须是 agent uid 拥有、非 group/world accessible 的 canonical absolute directory。

设备端 agent 在本地创建 inherited trace FD，并输出一行 result JSON 与 challenge 绑定的规范化 USTAR。运行 production candidate 时使用显式完成模式：

```text
mw_perf_ohos_capture_agent <agent-options> -- \
  /absolute/device/path/bin/ibmw_dds_managed_exact_candidate --exit-after-complete
```

使用 HDC 把 archive 和 result JSON 拉回 host，并保留启动 agent 时使用的完整 canonical request JSON。host archive 必须是当前用户拥有的普通非符号链接文件，link count 为 1，mode 为 0600；建议先放入 mode 0700 的私有 host 目录。然后运行安装后的 importer：

```bash
./install/bin/mw-perf-collect import-ohos-capture \
  --archive /absolute/host/capture-<request-challenge>.tar \
  --result /absolute/host/agent-result.json \
  --request /absolute/host/request.json \
  [--output /absolute/host/import-summary.json]
```

importer 严格验证 canonical USTAR、request challenge、archive/tree hash、agent result/manifest、child PID/start ticks、meta/seal 及 stage/exact semantics。`--output` 省略时 stdout 输出单个 bounded JSON；指定时以 0600 独占创建，拒绝覆盖已有文件。

该流程是 remote candidate，不是正式 OHOS P0a：summary 固定 `formal_p0a=false`，不会生成 `directory_finalization.json`，也不提供 trusted remote contract/campaign、runtime image attestation、fault/rollback、metric 或 comparison 证据。`metric_availability=unsupported`、`valid_for_metric=false`、`valid_for_comparison=false`，不得从 capture 数量或 exact correlation 发布性能结论。

## 9. 输出产物解释

| 路径 | 内容 |
|---|---|
| `manifest.json` | run ID、adapter、mode、PID identity、状态、合同 hash、capability/provenance 摘要 |
| `validation.json` | collection/metric/comparison 三层 verdict、规则和失败码 |
| `report.md` | 单轮状态、Core Metrics/latency 摘要和比较有效性 |
| `environment/host.json` | 主机 Python 与关键环境变量 |
| `environment/ros2.json` | ROS 环境和版本观测 |
| `environment/ibmw.json` | IBMW source/build/provenance |
| `topology/*.json` | ROS Graph 或 IBMW Transport/runtime 摘要 |
| `capability/*.json` | Adapter readiness、支持与降级 |
| `raw/process.jsonl` | 每次采样的进程 CPU、PSS/RSS、线程与 start ticks |
| `raw/network.jsonl` | 指定接口 RX/TX bytes/packets 累积计数 |
| `raw/latency.jsonl` | producer-owned ROS latency artifact |
| `raw/core_metrics.jsonl` | IBMW Core Metrics 归档 |
| `raw/runtime_images.json` | 实际运行 image 与声明的匹配证据 |
| `logs/` | managed SUT stdout/stderr |

网络字段是接口累计计数，需要用采样差值除以时间窗口；它不是 DDS/RTPS wire bytes。PSS 为正式内存口径，RSS 仅辅助诊断，共享页场景不要直接对 RSS 求和作结论。

## 10. 如何做 A/B/C

### 10.1 通用 collector 边界

`compare` 只消费已经具备比较元数据的运行目录。除有效的单轮 validation 外，每个 manifest 还必须由上游集成流程在生成时写入一致的 `scenario_contract_hash`、resource profile、comparison block、sequence index 和 order policy。当前通用 `run` 不会自动生成这些 block/order 元数据；版本化 `mw_perf.comparison-plan.v1` 会 fail-closed 检查每个 block 恰好包含 A、B、C 各一个 lane，并校验 block/lane identity、合同与 scope/order 元数据、run/path 唯一性及 validation 新鲜度。未声明该 schema 的旧 `runs` plan 仅保留 MVP 兼容。含 shm 通道 S 的比较使用 `mw_perf.comparison-plan.v2`，通道集合为 A、B、C 或 A、B、S，并校验每条通道的 latency 标签，见 `tools/mw_perf_collect/README.md` 与 `src/samples/cpp/transport_benchmark/abc_interop/README.md`。因此不能把任意三个普通 run 直接交给 `compare`，也不要通过手工修改 manifest 绕过 provenance。

对于已经由集成流程生成的目录，将路径写入 plan：

```json
{
  "runs": [
    "runs/A/<run-id>",
    "runs/B/<run-id>",
    "runs/C/<run-id>"
  ]
}
```

然后执行：

```bash
$COLLECTOR compare --plan comparison-plan.json \
  --valid-blocks 1 --order balanced --output comparison
```

当前 `compare` 只重校验已有运行并生成 `comparison.json` MVP；不会替你启动三条 lane、补齐比较元数据，也不计算完整 latency ratio、置信区间或正式 pass/fail。

### 10.2 独立 runner 原型

固定 4 KiB interop 场景由独立 runner 执行。先在已 source ROS 2 环境中构建三条 lane：

```bash
cmake -S . -B ./build \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_BENCHMARKS=ON \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ICEORYX_EXTENSION=ON
cmake --build ./build --target ibmw_abc_matrix --parallel
```

`ibmw_abc_matrix` 仅在 A/B/C 三组 endpoint 和 RouDi target 均可用时存在。源码依赖模式使用并构建 `iox-roudi`；`IBMW_ICEORYX_USE_PACKAGE=ON` 时识别 package 导出的 `iceoryx_posh::iox-roudi`，其可执行位置写入 `<build>/ibmw_abc_roudi_path.txt`。若 CMake 未生成该 target，应先补齐配置输出中列出的缺失依赖，不能用仅构建可用 lane 的 `ibmw_abc_smoke` 代替完整矩阵前置。然后执行：

```bash
python3 src/samples/cpp/transport_benchmark/abc_interop/run_matrix.py \
  --build ./build --blocks 5 --output ./abc-results
```

runner 默认读取上述 CMake 路径文件；需要覆盖时传入 `--roudi /absolute/iox-roudi`，或设置 `IBMW_ROUDI_BIN`。命令中的相对路径均按当前工作目录解释。

该 runner 使用 A=ROS 2、B=IBMW DDS compatible、C=IBMW iceoryx ordinary `Send()`，并验证固定 CSV、subscriber status、C publisher completion、freshness 与进程退出。C 不具备 DDS 协议级 reliability/discovery，结果必须单列 semantic warning。

独立 runner 的 `summary.json` 不是 `mw-perf-collect compare` 的正式统计输出；二者目前是两个原型边界，不能声称已经形成任意场景的一键 A/B/C 产品能力。

## 11. 结果解读

先看 validity，再看数值：

1. `valid_for_collection=false`：基础 manifest 或 raw samples 不完整，停止解读；
2. `valid_for_metric=false`：readiness、运行状态、required artifact 或 provenance 失败，该轮指标不可用；
3. `valid_for_comparison=false`：不能进入 A/B/C 聚合，即使局部数字看起来合理；
4. `unsupported` 不等于 0，表示该方法无法提供此指标；
5. `stale` 表示读到旧 artifact，不能归入本轮；
6. `partial` 只作诊断，不得进入正式比较。

比较时固定 payload、QoS、offered load、进程边界和资源范围。B/A 用于兼容迁移成本，C/B 或 C/A 只有在语义等价时才可直接比较；非等价 C 只展示 Pareto 结果。

## 12. 通用性与约束

- 工具是 Linux 单机 MVP，不提供多 host 时钟同步或单向时延；
- ROS Adapter 对 ROS 发行版和 RMW 保持结构通用，但当前验证基线不能自动外推到所有组合；
- producer-owned latency 只适用于能写严格 artifact 的受控 producer；真实业务没有序号/时间戳时不提供逐消息 E2E；
- IBMW External Adapter 可用于不同 Transport，但 topology 只是配置摘要，不是完整运行时 Graph；
- Core Metrics kind 取决于实际接入路径，缺失 kind 不能推断为计数为 0；
- collector 不自动扣除自身开销，应将 harness 与 solution 资源分开设计；
- required provenance 会因源码、untracked source、二进制或 CMake cache 变化失败，运行期间不要重编译或改源码。

## 13. 故障排查

### `doctor` 显示 ROS degraded

确认已 source ROS 环境、`ros2 -h` 可执行，并检查 `ROS_DOMAIN_ID`、`RMW_IMPLEMENTATION`。工具不会在 ROS 缺失时伪造 Graph。

### readiness timeout

确认 topic 名完全一致、目标 domain/RMW 相同、SUT 在 timeout 内存活。查看 `capability/ros2.json` 中 attempts、first_seen 和 failure_reason。

### `PID identity changed` 或采样期退出

目标进程已退出或 PID 被复用。managed shell 应以 `exec` 让真正的长期 SUT 成为 leader；不要让 wrapper 立即退出。

### PSS 为 `null`

当前用户无权读取 `smaps_rollup`，或内核未提供该文件。不要用 0 替代；调整运行用户/容器权限后重跑。

### latency artifact invalid/stale

删除或换用每轮独立输出路径，确认 producer 读取 collector 注入的 run ID，记录自身 PID/start ticks，并在 SIGTERM 后写唯一 completion 末行和换行。attached required 不会通过。

### Core Metrics unsupported/stale/invalid

检查 `IBMW_CORE_METRICS=counters`、输出目录可写、Adapter 指向同一目录，以及 SUT 是否正常 Engine shutdown。SIGKILL、旧文件未变化、末行截断、非递增 sequence 或 source/archive hash 不同都会失败。

### required provenance 失败

确认 `scenario_contract` 和 `runtime_images` declarations 非空，`build_dir` 中存在 CMake cache，声明的是实际 SUT 和加载库。`maps` 声明必须对应实际可执行映射而不是只读 mmap；检查失败证据中的 permissions、PID identity、device/inode、路径、大小和 hash。采集期间不要重编译、切分支或修改源码。

### 结果目录的信任边界

当前运行目录是自包含证据包，用于检测缺失、陈旧、内部不一致和采集后 source/build/binary 变化。目录内 hash 不是数字签名，也不抵抗拥有整个结果目录写权限的攻击者同时重写 `process.jsonl`、runtime evidence 和 manifest。需要主动防伪时，部署方必须在目录之外增加受保护密钥签名、远端证明或不可变存储；不得把当前 `valid_for_comparison=true` 描述成密码学真实性证明。

### network 为空

检查接口名和 namespace。回环测试通常使用 `lo`；跨网卡测试使用实际接口。不存在的接口不会产生伪造的零计数，但当前会被静默跳过且不使validation失败；必须确认`raw/network.jsonl`中的`interfaces`包含目标名称。

## 14. 可复现 ROS2 案例

先构建包含 `ibmw_abc_ros2_endpoint` 的 benchmark，并直接运行一次 `ibmw_abc_ros2_endpoint --help` 或短消息 smoke，确认其动态库来自目标 ROS/系统环境而不是冲突的 Conda/toolchain 运行库。创建 `ros2-abc.json`：

```json
{
  "schema_version": 1,
  "run": {"warmup_seconds": 1, "sample_seconds": 8, "stop_timeout_seconds": 5},
  "sut": {
    "mode": "managed",
    "command": ["/bin/sh", "src/samples/cpp/transport_benchmark/abc_interop/run_a_managed.sh"],
    "working_directory": ".",
    "env": {
      "IBMW_ABC_ROS2_ENDPOINT": "./build/ibmw_abc_ros2_endpoint",
      "IBMW_ABC_LATENCY_JSONL": "/tmp/ibmw-perf-collection/ros2-abc-latency.jsonl",
      "IBMW_ABC_MESSAGE_COUNT": "3000",
      "IBMW_ABC_WARMUP_MESSAGES": "50",
      "IBMW_ABC_HZ": "1000",
      "ROS_DOMAIN_ID": "45"
    }
  },
  "collectors": {
    "process": {"interval_ms": 500, "include_descendants": true},
    "network": {"interfaces": ["lo"]}
  },
  "scenario_contract": {
    "id": "abc-ros2-4k-v1",
    "deployment": "single_host_cross_process_1to1",
    "topic": "/mw_perf/payload_4kb",
    "payload_bytes": 4096,
    "qos": "reliable_volatile_keep_last_8192",
    "measured_messages": 3000
  },
  "adapters": {
    "ros2": {
      "readiness_timeout_seconds": 10,
      "readiness_poll_interval_seconds": 0.2,
      "topics": [{"name": "/mw_perf/payload_4kb", "count": 1}],
      "latency_artifact": {"path": "/tmp/ibmw-perf-collection/ros2-abc-latency.jsonl", "required": true}
    }
  }
}
```

执行：

```bash
mkdir -p /tmp/ibmw-perf-collection
source /opt/ros/humble/setup.bash
$COLLECTOR doctor --adapter ros2 --config ros2-abc.json
$COLLECTOR run --adapter ros2 --config ros2-abc.json --output runs/ros2-abc
$COLLECTOR validate runs/ros2-abc/<run-id>
$COLLECTOR report runs/ros2-abc/<run-id>
```

这个案例的 shell 将 publisher 作为后代启动，并 `exec` subscriber 作为 collector leader；subscriber 负责生成严格 latency artifact 和 completion。

## 15. 可复现 IBMW 案例

以下案例使用已有 retained/direct loan E2E 样例的 managed helper。先构建：

```bash
cmake -S . -B <build-dir> \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_ICEORYX_EXTENSION=ON \
  -DIBMW_BUILD_RETAINED_LOAN_E2E=ON \
  -DIBMW_ENABLE_CORE_METRICS=ON \
  -DIBMW_ENABLE_REPORTER_V1=ON
cmake --build <build-dir> --target retained_loan_e2e --parallel
```

再创建`ibmw-core.json`。把`<build-dir>`替换为实际构建目录。源码依赖模式构建 `retained_loan_e2e` 时会同时构建 RouDi；package 模式使用导入的 `iceoryx_posh::iox-roudi`。helper 的最后一个 RouDi 参数可省略，此时依次读取 `IBMW_ROUDI_BIN`、`<build-dir>/retained_loan_e2e_roudi_path.txt`，最后兼容源码构建目录的 `<build-dir>/iox-roudi`。动态metrics必须写在IBMW源码树外，避免required provenance在运行期间因新增未跟踪文件而变化：

```json
{
  "schema_version": 1,
  "run": {"warmup_seconds": 1, "sample_seconds": 12, "stop_timeout_seconds": 10},
  "sut": {
    "mode": "managed",
    "command": [
      "/bin/sh",
      "src/samples/cpp/retained_loan_e2e/run_metrics_managed.sh",
      "retained",
      "<build-dir>",
      "/tmp/ibmw-perf-collection/retained-core"
    ],
    "working_directory": "."
  },
  "collectors": {
    "process": {"interval_ms": 500, "include_descendants": true},
    "network": {"interfaces": ["lo"]}
  },
  "scenario_contract": {
    "id": "ibmw-retained-loan-e2e-v1",
    "deployment": "single_host_cross_process",
    "transport": "iceoryx",
    "path": "retained_receive",
    "measurement_lane": "normalized_benchmark"
  },
  "adapters": {
    "ibmw": {
      "root": ".",
      "build_dir": "<build-dir>",
      "core_metrics": {"directory": "/tmp/ibmw-perf-collection/retained-core", "required": true},
      "provenance": {
        "required": true,
        "sut_executable": "<build-dir>/retained_loan_e2e",
        "artifact_binaries": [
          "<build-dir>/retained_loan_e2e",
          "<resolved-roudi-path>",
          "<build-dir>/libibmw_iceoryx_transport.so"
        ],
        "runtime_images": [
          {"role": "e2e", "name": "retained_loan_e2e", "source": "exe", "expected_path": "<build-dir>/retained_loan_e2e"},
          {"role": "roudi", "name": "iox-roudi", "source": "exe", "expected_path": "<resolved-roudi-path>"},
          {"role": "transport", "name": "iceoryx", "source": "maps", "expected_path": "<build-dir>/libibmw_iceoryx_transport.so"}
        ]
      }
    }
  }
}
```

执行：

```bash
mkdir -p /tmp/ibmw-perf-collection
$COLLECTOR doctor --adapter ibmw --config ibmw-core.json
$COLLECTOR run --adapter ibmw --config ibmw-core.json --output runs/ibmw-core
$COLLECTOR validate runs/ibmw-core/<run-id>
$COLLECTOR report runs/ibmw-core/<run-id>
```

该样例的publisher固定运行约7秒，`sample_seconds`必须覆盖其完成时间。collector会验证artifact、finalization和provenance，但当前不会验证该样例的业务完成语义；用户还必须检查`subscriber.log`中的`RETAINED_E2E_SUBSCRIBER_PASS`和publisher完成证据。预期结果是External P0 artifact完整、`raw/core_metrics.jsonl`为`supported`、source/archive hash一致且三层validity均为true。若只要外部指标，将`IBMW_CORE_METRICS`设为`off`并移除required `core_metrics`。

### 15.1 shm 版（`shm_retained_loan_e2e`）

自研 shm 传输有同样的 E2E 验证器。`shm_retained_loan_e2e` 编译同一份 `retained_loan_e2e.cc`（定义 `IBMW_RETAINED_E2E_SHM`），负载与 iceoryx 版相同。它直接链接 shm_ext（`libibmw_shm_transport.so`），不需要 RouDi。Reporter V1 的 kind 为 `direct_shm_loan`、`retained_shm_receive`，driver 标签为 `shm`。iceoryx 版目标、helper 与 `assert_metrics.py` 不变。构建：

```bash
cmake -S . -B <build-dir> \
  -DIBMW_BUILD_SHM_EXTENSION=ON \
  -DIBMW_BUILD_SHM_RETAINED_LOAN_E2E=ON \
  -DIBMW_ENABLE_CORE_METRICS=ON \
  -DIBMW_ENABLE_REPORTER_V1=ON
cmake --build <build-dir> --target shm_retained_loan_e2e --parallel
```

managed helper 为 `run_shm_metrics_managed.sh direct|retained <build-dir> <metrics-dir>`，没有 RouDi 参数。每次运行用 `mktemp -d /tmp/ibR.XXXXXX` 创建私有 `IBMW_SHM_ROOT`，并设置唯一的 `IBMW_SHM_DOMAIN`。helper 在创建该目录之前就装好 TERM/INT/HUP 处理；收到这些信号后结束子进程并删除该目录，不使用 `/dev/shm`。SIGKILL 杀死 helper 时无法清理，该目录会留下。publisher 在后台运行并被等待，所以发布期间的停止请求不用等它结束。只有在 leader 的前台 `sleep 1` 期间，只发给 leader 的信号才最多晚 1 s 处理；collector 对进程组发的 TERM 会直接结束这个 sleep。

helper 的退出码：

- 正常停止时为 0。子进程以 0 或 129/130/143（停止请求造成）结束都算正常。
- publisher 非零退出时，helper 立即以同一退出码退出。
- direct 或 subscriber 以其他退出码结束时，helper 在停止时把失败写到 stderr，并以第一个失败的退出码退出。
- 创建私有目录期间被停止时为 2。

配置与上例相比只有以下不同：

```json
{
  "sut": {
    "mode": "managed",
    "command": [
      "/bin/sh",
      "src/samples/cpp/retained_loan_e2e/run_shm_metrics_managed.sh",
      "retained",
      "<build-dir>",
      "/tmp/ibmw-perf-collection/retained-shm-core"
    ],
    "working_directory": "."
  },
  "scenario_contract": {
    "id": "ibmw-retained-loan-shm-e2e-v1",
    "deployment": "single_host_cross_process",
    "transport": "shm",
    "path": "retained_receive",
    "measurement_lane": "normalized_benchmark"
  },
  "adapters": {
    "ibmw": {
      "root": ".",
      "build_dir": "<build-dir>",
      "core_metrics": {"directory": "/tmp/ibmw-perf-collection/retained-shm-core", "required": true},
      "provenance": {
        "required": true,
        "sut_executable": "<build-dir>/shm_retained_loan_e2e",
        "artifact_binaries": [
          "<build-dir>/shm_retained_loan_e2e",
          "<build-dir>/libibmw_shm_transport.so"
        ],
        "runtime_images": [
          {"role": "e2e", "name": "shm_retained_loan_e2e", "source": "exe", "expected_path": "<build-dir>/shm_retained_loan_e2e"},
          {"role": "transport", "name": "shm", "source": "maps", "expected_path": "<build-dir>/libibmw_shm_transport.so"}
        ]
      }
    }
  }
}
```

`run`、`collectors` 与上例相同，`sample_seconds` 同样要覆盖 publisher 约 7 秒的运行时间。collector 的 ibmw adapter 不区分传输：provenance required 时，`runtime_images` 不能为空；每一项都必须出现在 `artifact_binaries` 中，并在采样进程的 `/proc/<pid>/exe` 或 `/proc/<pid>/maps` 中被观察到。shm 版没有 RouDi 项。shm_ext 是可执行文件的 `DT_NEEDED` 共享库，所以 transport 项用 `maps` 声明 `libibmw_shm_transport.so`。去掉这一项和对应的 artifact 也合法，但那样 provenance 就不记录传输库。运行结束后要做三项检查，缺一不可。

1. 检查 manifest 的 `sut_returncode` 是否为 0。collector 不会因 helper 非零退出而判定运行无效：实测 publisher 失败时，`sut_returncode` 为 3，但 `run_status` 仍是 `completed`，valid_for_collection/metric/comparison 均为 true，也没有失败规则。失败原因在 `logs/sut.stderr.log` 中。
2. 检查 `subscriber.log` 中的 `RETAINED_E2E_SUBSCRIBER_PASS` 与 `publisher.log` 中的 `RETAINED_E2E_PUBLISHER_DONE`。门禁本身发现不了被截断的运行：`sample_seconds` 太短、发布期间就被停止时，实测 delivered 只有 17、18，门禁仍然通过，只有缺失的 `RETAINED_E2E_PUBLISHER_DONE` 能说明问题。
3. 运行门禁：

```bash
python3 src/samples/cpp/retained_loan_e2e/assert_shm_metrics.py retained \
  /tmp/ibmw-perf-collection/retained-shm-core/metrics.jsonl
```

`assert_shm_metrics.py [--driver NAME] [--check-run] direct|retained metrics.jsonl` 包含 `assert_metrics.py` 的全部检查，并且更严格：

- direct 的 attempt 3、success 2、fail 1、publish 1、release 1 取自 `direct.shm.metrics.json` 并逐项精确比较，internal_retry 须大于 0。
- retained 要求 receive_delivered ≥ 2，callback_entered ≤ receive_delivered。release token 分配失败的样本会被归还而不进回调，所以这里不要求相等。
- `--check-run` 用于 `check_shm_metrics.sh`：订阅端在第一次恢复回调后就退出，因此额外要求 receive_delivered = callback_entered = 2，且 limit_drop < 120。实测为 23–24；归还延迟到 7 s 超时的竞争曾得到 229。managed 运行的订阅端一直存活到 TERM，完整运行实测 delivered 为 216，不能加 `--check-run`。
- 该 kind 的每条记录的 driver 标签都必须是 `shm`；shm 以 `driver_name: iceoryx` 原位替换时，用 `--driver iceoryx` 指定。
- 每条记录的 value 必须是非负的普通 int，布尔值、浮点数、字符串都被拒绝。有 `cumulative.value` 时它必须等于 value。该 kind 的记录只能属于一个 topic/type。
- 真实文件每个 metric 只有一条记录。同一 metric 有多条记录时，只当作累计序列接受：snapshot_seq 互不相同，值不下降（计量值 `outstanding_current` 除外），检查最后一个快照的值。其他重复一律拒绝。
- 失败时以 `SystemExit` 返回非零，不依赖 `assert`，所以在 `python -O` 下依然有效。

场景契约见 `direct.shm.metrics.json`、`retained.shm.metrics.json`。

不用 collector 时，`check_shm_metrics.sh <build-dir> <work-dir>` 依次运行 direct 与 retained 负载。它不设 `MW_PERF_MANAGED`，所以进程自行退出，并在关闭时写出最终快照 `metrics.jsonl`。脚本检查 PASS 标记并运行 `assert_shm_metrics.py --check-run`，任一失败即返回非零。负载都在后台运行并被等待，因此脚本收到 TERM/INT/HUP（如 ctest 超时）时会结束子进程、删除私有根并返回非零。唯一的前台命令是 publisher 之前的 `sleep 1`，此时最多晚 1 s 处理。SIGKILL 杀死脚本时无法清理。开启 `IBMW_BUILD_TESTS` 与 `IBMW_ENABLE_REPORTER_V1` 时，它注册为 ctest `shm_retained_loan_e2e_metrics`。
