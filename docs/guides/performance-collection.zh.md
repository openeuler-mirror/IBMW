<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# ROS 2 与 IBMW 性能采集指南

## 1. 适用对象

本指南适用于需要在 Linux 单机上采集以下证据的开发者和性能工程师：

- ROS 2 业务或受控 benchmark 的环境、Graph、进程资源、网络与逐消息 latency artifact；
- IBMW 业务的外部环境、构建/Transport 信息、进程资源与 Core Metrics artifact；
- 已分别完成 A、B、C 运行后，对结果目录做一致性检查和最小聚合。

工具不会替你选择公平场景，也不会自动把非等价 Transport 变成 DDS 等价方案。正式结论应先阅读[设计基线](../architecture/performance-observability-design.zh.md)。

## 2. 当前已实现能力

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

## 3. 前置条件

- Linux，且可读取目标进程的 `/proc`；PSS 需要读取 `/proc/<pid>/smaps_rollup`；
- Python 3；JSON 配置不依赖第三方包，YAML 配置需要 PyYAML；
- ROS2 Adapter 需要先 source 对应 ROS 环境，并确保 `ros2` CLI 可用；
- IBMW Adapter 需要可读的仓库、构建目录和被测二进制；
- Core Metrics 需要被测 IBMW 构建已包含该能力，且 SUT 能正常走 Engine shutdown；
- 容器内采集时需确认 `/proc` 和 `/sys/class/net` 是目标 namespace 的视图。当前不存在的网络接口会被静默跳过，不构成自动 gate；运行后必须检查 `raw/network.jsonl` 中目标接口确实存在。

工具不执行交互式 sudo。权限不足的指标会降级或使 required gate 失败。

## 4. managed 与 attached

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

## 5. 完整 JSON 配置

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

## 6. ROS2 使用流程

### 6.1 doctor

```bash
source /opt/ros/humble/setup.bash
$COLLECTOR doctor --adapter ros2 --config ros2-run.json
```

`doctor` 不启动 SUT。重点检查 `status`、ROS 环境、ROS 2 CLI 可达性、RMW 以及公共 `/proc`、sysfs network 和 YAML 能力。ROS 发行版从 `ROS_DISTRO` 记录；工具不调用 Humble 不支持的 `ros2 --version`。

### 6.2 run

```bash
$COLLECTOR run --adapter ros2 --config ros2-run.json --output runs/ros2
```

如果命令不写入配置：

```bash
$COLLECTOR run --adapter ros2 --config ros2-run.json \
  --output runs/ros2 --override-command -- ./build/my_ros2_sut
```

ROS Adapter 只做 Graph/readiness 和 artifact 校验。逐消息 latency 不是 `ros2 topic hz` 推导值；producer 必须输出 `mw_perf.latency.v1` JSONL。

### 6.3 validate

```bash
$COLLECTOR validate runs/ros2/<run-id>
```

检查 `valid_for_collection`、`valid_for_metric`、`valid_for_comparison` 和 `failure_codes`。required latency 的 completion、运行身份、hash 或优雅 finalization 任一失败，结果不可比较。

### 6.4 report

```bash
$COLLECTOR report runs/ros2/<run-id>
```

该命令从现有 artifact 重建 `validation.json` 和 `report.md`，不重跑 SUT。当前报告是单轮摘要，不是正式 A/B/C 统计报告。

## 7. IBMW External 与 Core Metrics

### 7.1 仅 External P0

不要求 Core Metrics 时，删除 `core_metrics` 或设为 optional，并执行：

```bash
$COLLECTOR doctor --adapter ibmw --config ibmw-external.json
$COLLECTOR run --adapter ibmw --config ibmw-external.json --output runs/ibmw
```

你仍会得到 source/build/Transport 摘要、进程资源、网络和 capability。Core Metrics 显示 `unsupported` 不代表 External P0 失败。

### 7.2 开启 Core Metrics

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

当前 counters 性能门尚未通过，不应在生产环境中默认打开，也不能用内部计数替代 A/B/C 公共外部指标。

## 8. 输出产物解释

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

## 9. 如何做 A/B/C

### 9.1 通用 collector 边界

`compare` 只消费已经具备比较元数据的运行目录。除有效的单轮 validation 外，每个 manifest 还必须由上游集成流程在生成时写入一致的 `scenario_contract_hash`、resource profile、comparison block、sequence index 和 order policy。当前通用 `run` 不会自动生成这些 block/order 元数据；`compare` MVP也不检查每个block是否包含完整A/B/C lane集合。因此不能把任意三个普通run直接交给`compare`，也不要通过手工修改manifest绕过provenance。

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

### 9.2 独立 runner 原型

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

runner 默认读取上述 CMake 路径文件；需要覆盖时传入 `--roudi /path/to/iox-roudi`，或设置 `IBMW_ROUDI_BIN`。命令中的相对路径均按当前工作目录解释。

该 runner 使用 A=ROS 2、B=IBMW DDS compatible、C=IBMW iceoryx ordinary `Send()`，并验证固定 CSV、subscriber status、C publisher completion、freshness 与进程退出。C 不具备 DDS 协议级 reliability/discovery，结果必须单列 semantic warning。

独立 runner 的 `summary.json` 不是 `mw-perf-collect compare` 的正式统计输出；二者目前是两个原型边界，不能声称已经形成任意场景的一键 A/B/C 产品能力。

## 10. 结果解读

先看 validity，再看数值：

1. `valid_for_collection=false`：基础 manifest 或 raw samples 不完整，停止解读；
2. `valid_for_metric=false`：readiness、运行状态、required artifact 或 provenance 失败，该轮指标不可用；
3. `valid_for_comparison=false`：不能进入 A/B/C 聚合，即使局部数字看起来合理；
4. `unsupported` 不等于 0，表示该方法无法提供此指标；
5. `stale` 表示读到旧 artifact，不能归入本轮；
6. `partial` 只作诊断，不得进入正式比较。

比较时固定 payload、QoS、offered load、进程边界和资源范围。B/A 用于兼容迁移成本，C/B 或 C/A 只有在语义等价时才可直接比较；非等价 C 只展示 Pareto 结果。

## 11. 通用性与约束

- 工具是 Linux 单机 MVP，不提供多 host 时钟同步或单向时延；
- ROS Adapter 对 ROS 发行版和 RMW 保持结构通用，但当前验证基线不能自动外推到所有组合；
- producer-owned latency 只适用于能写严格 artifact 的受控 producer；真实业务没有序号/时间戳时不提供逐消息 E2E；
- IBMW External Adapter 可用于不同 Transport，但 topology 只是配置摘要，不是完整运行时 Graph；
- Core Metrics kind 取决于实际接入路径，缺失 kind 不能推断为计数为 0；
- collector 不自动扣除自身开销，应将 harness 与 solution 资源分开设计；
- required provenance 会因源码、untracked source、二进制或 CMake cache 变化失败，运行期间不要重编译或改源码。

## 12. 故障排查

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

## 13. 可复现 ROS2 案例

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

## 14. 可复现 IBMW 案例

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
