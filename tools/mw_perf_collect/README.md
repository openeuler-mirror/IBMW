# mw-perf-collect

`mw-perf-collect` 是一个无侵入、标准库优先的中间件性能证据采集工具。它提供通用进程、PSS、CPU、线程和网络采样，并通过适配器补充 ROS 2 或 IBMW 环境、拓扑与能力信息。

## 从源码运行

以下命令均从 IBMW 仓库根目录执行：

```bash
PYTHONPATH=tools python3 -m mw_perf_collect doctor --adapter ros2
PYTHONPATH=tools python3 -m mw_perf_collect doctor --adapter ibmw --config config.json
PYTHONPATH=tools python3 -m mw_perf_collect run --adapter generic --config config.json --output runs
PYTHONPATH=tools python3 -m mw_perf_collect validate --profile standard runs/<run-id>
PYTHONPATH=tools python3 -m mw_perf_collect validate --profile p0a runs/<run-id>
PYTHONPATH=tools python3 -m mw_perf_collect report runs/<run-id>
```

也可在工具父目录运行模块：

```bash
cd tools
python3 -m mw_perf_collect --help
```

安装态 CLI 矩阵（不依赖真实 daemon）
--------------------------------------

仓内安装后 smoke 会在隔离的 `PYTHONPATH`/`PYTHONHOME` 环境中执行：
对现有 run fixture 运行 `validate <fixture>`、`report <fixture>`，并运行
`compare --plan <fixture>`。测试确认命令从安装目录加载，validate/compare 的
JSON 产物可解析、report 产生 Markdown，且新生成结果带有 `schema_audit`。
业务失败状态（例如 manifest 缺失或有效 block 数不足）按 CLI 合同检查其
退出码和 `failure_codes`，不会被当作安装失败；旧兼容 fixture 可没有
`schema_audit`。该矩阵只覆盖 CLI 加载、fixture 验证和产物格式，不启动或
探测真实 ROS 2/IBMW daemon，也不证明性能指标为 PASS。

## CMake 安装后运行

配置时保持 `IBMW_INSTALL=ON`，安装到自定义前缀后直接运行安装命令：

```bash
cmake -S . -B build -DIBMW_INSTALL=ON
cmake --build build
cmake --install build --prefix "$PWD/install"
./install/bin/mw-perf-collect --help
./install/bin/mw-perf-collect doctor --adapter generic
./install/bin/mw-perf-collect import-ohos-capture --archive /absolute/host/capture-<challenge>.tar --result /absolute/host/agent-result.json --request /absolute/host/request.json --output /absolute/host/import-summary.json
./install/bin/mw-perf-collect materialize-ohos-p0a-candidate --archive /absolute/host/capture-<challenge>.tar --result /absolute/host/agent-result.json --request /absolute/host/request.json --destination /absolute/host/new-candidate
```

`import-ohos-capture` 只导入已复制到本机的 OHOS capture，不接受远端 shell、设备路径
或启动命令。`result` 和 `request` 必须是非符号链接的普通 JSON 文件，重复 JSON key
会被拒绝。省略 `--output` 时 stdout 仅输出一条有界 JSON；指定输出时以 0600 权限发布
canonical JSON 且不覆盖已有文件。该摘要只含语义状态、stage/exact 数量、hash 和来源，
始终声明 `formal_p0a=false`，不会写 `directory_finalization.json`，也不声明 metric 或
comparison 有效。

`materialize-ohos-p0a-candidate` 是独立的 v2 exact-anchor 路径。它只接受全新的绝对
destination，以 0700 staging 目录重新验证并导入 raw trace，生成 collector-owned v8
candidate artifacts，通过 `validate --profile p0a` 后以 no-replace rename 原子发布。失败
或目标冲突不会留下 completed tree；该输出仍明确为 candidate-only、metric unsupported
和 `formal_p0a=false`。

安装后双适配器 smoke（不需要 ROS 2 或 IBMW daemon；缺失外部环境应按合同降级）：

```bash
./install/bin/mw-perf-collect doctor --adapter ros2 --config doctor.json
./install/bin/mw-perf-collect doctor --adapter ibmw --config doctor.json
```

两条命令都应返回可解析 JSON，顶层 `status` 为 `ready`、`degraded` 或
`unsupported`；该检查同时覆盖安装后的 `adapters/` 模块布局。

安装包同时携带 `schemas/`、`contracts/` 及运行时 Python resources。`validate`、`report`
和 `compare` 启动时会校验这些内置资源的 SHA-256；资源缺失或被篡改时返回非零状态，
不会把证据默认为有效。安装后 smoke 覆盖资源布局及篡改 fail-closed 行为。

Python 包安装在 GNUInstallDirs 的 `libexec/ibmw` 布局中；安装的命令会自动定位该包，无需设置 `PYTHONPATH`，也不依赖 PyInstaller 或 pip。

`sut.mode` 可为 `managed`（工具启动并在采集后停止进程）或 `attached`（附加已有 PID，绝不停止目标）。PSS 来自 `/proc/<pid>/smaps_rollup`；不可访问时输出 `null`，不会伪造为零。

工具记录目标 PID 的 Linux `start_time_ticks`，并在 warmup 和每次采样时核对，避免 PID 复用。managed 模式还会持续记录已观察到的后代进程身份；停止进程组后会对这些进程逐一进行 best-effort TERM/KILL（包括逃逸到新 session 的后代）。适配器 readiness 与拓扑快照在 warmup 完成后采集。

## 适配器

- `generic`：仅使用公共采集器。
- `ros2`：被动调用 ROS 2 CLI 获取环境和 graph snapshot；未安装 ROS 时明确降级为 `unsupported`，不会创建 observer/subscription 或合成指标。
- `ibmw`：从源码版本、CMake cache、transport 配置和可选 core capability 文件进行外部只读发现。未提供 core capability 时 external P0 继续工作并明确标记 unsupported。

IBMW 可通过 `adapters.ibmw.core_metrics` 配置 Core Metrics JSONL 文件或目录（目录中自动使用 `metrics.jsonl`），例如 `{"directory":"/tmp/ibmw-metrics","required":false}`。工具在运行前记录文件身份，并仅在 managed SUT 停止/final flush 后复制变化后的文件到 `raw/core_metrics.jsonl`；attached 模式绝不停止 SUT。Core Metrics artifact manifest 的 `provenance` 记录 source/archive SHA-256、baseline/final 文件 metadata、run_id、adapter、build、source root 与 graceful finalization，报告展示关键字段。状态会记录为 `supported`、`unsupported`、`stale` 或 `invalid`。默认 optional；仅 `required: true` 会令 validate/compare 对 artifact 与 provenance fail-closed。

正式 IBMW E2E 应设置 `adapters.ibmw.build_dir`、`provenance.required=true`、`provenance.sut_executable`、非空 `provenance.artifact_binaries`（wrapper command 不算实际 SUT），以及非空 `provenance.runtime_images` declarations。required provenance 的 runtime image 门禁不可降级：run preflight 会拒绝缺少 declarations 的配置，validate/compare 始终要求完整的 `raw/runtime_images.json` 及 manifest evidence，不信任可删除的 `provenance.runtime_images_required` 诊断字段。`source: "maps"` 仅接受 `/proc/<pid>/maps` 中 permissions 格式合法且含 `x` 的可执行映射；证据记录原始 permissions，只读 data mmap 不能满足 required match。manifest 还会记录每个文件的 resolved path/device/inode/size/mtime/SHA-256、CMakeCache options/hash，以及源码 commit/branch/dirty 和工作树 diff SHA-256；不会保存完整 diff。required provenance 同时要求非空 `scenario_contract`，并保存 canonical 对象及其 hash。`validate` 会重新读取所有二进制、CMakeCache 和 git identity，任何篡改或源码变化均 fail-closed。未启用 required provenance 的旧配置保持兼容。

信任边界：运行目录是自包含证据包，fail-closed 用于检测缺失、陈旧、内部不一致和采集后环境变化；目录内 SHA-256 不是签名。工具不承诺抵抗拥有整个结果目录写权限并可同时重写采样、evidence、manifest 和 hash 的攻击者。需要主动防伪时，应在结果目录之外使用受保护密钥签名、远端证明或不可变存储。

适配器结果分别写入运行目录下的 `environment/`、`topology/`、`capability/`。指标记录使用规范字段 `metric_availability`；ROS 2 记录暂时也保留旧 `availability` 以兼容消费者。

ROS 2 readiness 会在有界时间内重复执行只读 CLI graph 查询，同时核对目标 PID 的 Linux start ticks，绝不创建 observer/subscription。可在 `adapters.ros2` 中用 `readiness_timeout_seconds`（默认 `10.0`）和 `readiness_poll_interval_seconds`（默认 `0.2`）配置轮询；也支持等价的 `readiness.timeout_seconds` / `readiness.poll_interval_seconds`，`topics` 接口保持不变。结果的 `readiness.evidence` 包含 `attempts`、各 topic 的 `first_seen`、`elapsed` 和 `failure_reason`（超时、进程退出、identity 不可读或 PID identity 变化）。

ROS 2 collector 仅从启动时可信 `PATH` 解析 `ros2`，或使用 `adapters.ros2.ros2_executable` 指定的绝对路径。collector 子进程固定使用该 PATH，移除 `PYTHONPATH`、`PYTHONHOME`、`LD_PRELOAD`、`LD_LIBRARY_PATH`，并仅从 `sut.env` 传入明确的 ROS 环境白名单。`p0a_proof` 或 `validation_instrumented` 运行必须配置 `required_runtime_libraries` 的预注册 SHA-256/build-id/SONAME 身份；mapped device/inode/SHA 只标记 `identity_verified`，仅期望身份匹配后才是 `capability_verified`。required tracing 还必须配置非空、格式合法且已枚举的 tracepoints；Humble 探测使用 `ros2 trace --list`，未安全执行临时 session probe 时 `session_command_usable=false`。任一 required preflight 条件失败都会在进入 `SAMPLE` 前将 run 标记为 `invalid`/`required_stop`。

ROS 2 逐消息 latency 是 **producer-owned contract**：受控 benchmark 或业务进程自行写出 JSONL，collector 只归档并验证 producer 提供的证据。managed 启动时工具自动注入 `MW_PERF_RUN_ID` 和 `MW_PERF_ARTIFACT_SCHEMA_VERSION`，producer 从环境读取本次 run 身份，并从 `/proc/self/stat` 读取自身 `producer_start_time_ticks`，无需用户同步。通过 `adapters.ros2.latency_artifact` 配置 path 或 directory（目录默认 `latency.jsonl`），默认 optional，`required: true` fail-closed。每个 artifact 固定单一 topic/type；数据行 schema 为 `mw_perf.latency.v1`，字段严格为 `schema/seq/send_ns/receive_ns/latency_ns/topic/type/payload_bytes/valid`。最后必须由 producer 写入唯一 completion trailer；**completion 必须包含 `clock_domain`，唯一 supported 值为 `producer_monotonic`**，字段严格为 `schema/kind/run_id/producer_pid/producer_start_time_ticks/record_count/topic/type/complete/clock_domain`，其中 `kind="completion"`、`complete=true`，count、topic/type 及运行身份必须与数据和 manifest 一致。缺少或伪造 completion、空行、截断、NaN、重复/倒序 seq、跨 topic/type 或公式不一致均拒绝；collector 不会自行推断 final snapshot。managed required 仅在 graceful finalization、identity 和 completion 全部有效时 supported；attached required fail-closed，attached optional 仅作 diagnostic。**legacy 或 unknown clock domain 只可用于诊断，不产生 metric 或 comparison 结论。**

## 配置与输出

JSON 配置无需额外依赖；YAML 需要 PyYAML。运行目录包含 `manifest.json`、`validation.json`、`report.md`、原始 JSONL 样本、日志及适配器证据。`schemas/` 提供版本化 config、result（manifest）和 validation JSON Schema v1 原型。

`validate` 采用 fail-closed 规则：manifest 必需字段及采样文件缺失都会失败。普通/legacy run 使用显式 `--profile standard`；P0a 使用 `--profile p0a`，该 profile 不依赖运行目录内可删除字段来启用，并要求主 manifest 的 `validation_profile=p0a` marker、canonical P0a manifest 与归档合同全部存在。P0a 合同由安装目录 `contracts/` 中的 versioned trusted registry 按 `registration_contract_version` 选择，运行目录中的 `registration/contract.json` 仅是必须逐 byte 一致的归档副本。`compare` 每次都会按 comparison plan 的 `validation_profile`（legacy 默认 `standard`）重新验证运行目录，并要求磁盘上的 `validation.json` 与实时结果完全一致。

`mw_perf.metric_qualification.v1` 是独立、不可变的派生轴，只接受外部 root 验证通过的 campaign v2，并仅限定 `dds.ordinary.nonplain.single_driver.sync_callback`。它从归档 raw trace 重新计算精确 latency sample，绑定 formal anchor、delivery、trace quality、边界、公式、ns 单位和 producer monotonic clock。该 artifact 仅声明 `metric_availability=supported`、`valid_for_metric=true`、`valid_for_comparison=false`；不修改 v8/v9 或 `validation.json` 的 `METRIC_AVAILABLE`，不包含 percentile、threshold、overhead、ranking、regression 或 performance acceptance 结论，也不能作为 `compare` 证据。

正式 A/B/C 比较使用版本化 `mw_perf.comparison-plan.v1`（schema 位于 `schemas/comparison-plan.schema.v1.json`）。plan 声明全局 `contract`、`profile`、`workload`、`resource_scope` 和 `order`，每个 block 必须恰好引用一次 A、B、C 三个 lane。`compare --plan plan.json` 会逐 block 重新验证 run，并 fail-closed 检查 lane、上述五类 scope/order 元数据及其与 plan 的一致性；输出 `valid_blocks`、`invalid_blocks[].reasons` 和 `failure_codes`。该 P0 gate 不计算聚合统计，也不输出正式胜负结论。未声明该 schema 的旧 `runs` plan 保持 MVP 兼容。

P0a contract v2 为 `raw/comm_trace` 启用 managed DDS binary trace。collection run 必须配置 `p0a_evidence.trace_mode` 为 `sampled` 或 `full`；collector 创建 mode `0700` 的目录并只向 SUT 传递目录 FD，覆盖 run/capability/path/topology/mode/sink 控制环境。producer 输出严格的 `*.meta.json`、256-byte header + 64-byte little-endian records 的 `*.ibct` 和 `*.seal.json`。SUT graceful exit 后 collector 校验 identity、hash、segment/ordinal、SampleKey stage、seal 与零 loss counters，再原子写入 `directory_finalization.json`。`.part`、链接、未知文件、缺 seal、SIGKILL 或 cleanup/error 都不会产生 `finalized=true`。attached P0a binary trace 不受支持；`stage_run` 的 dispatch 只能声明 `stage_only`，在 callback registration identity 实现前 `exact_run` fail-closed。该 instrumentation 在 manifest/P0a manifest/report 中标记 `runtime_changed=true`，同时可独立记录 `business_unchanged=true`；standard run 不启用此路径。

## 测试

在 IBMW 仓库根目录执行：

```bash
PYTHONPATH=tools python3 -m pytest tools/mw_perf_collect
```

其中 `tests/test_cli.py` 的安装后 smoke 会构造临时 CMake 安装布局，并分别执行
ROS 2 与 IBMW doctor，不依赖实际 daemon。
