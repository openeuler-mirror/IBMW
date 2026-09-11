# Transport Benchmark 方法

IBMW 使用统一的 payload registry、测量框架和百分位计算方式比较共享内存、
DDS 与 TCP 路径。性能数据依赖硬件和运行环境；应用决策应以目标平台上的可复现
测量为准。

## 构建与运行

`full` preset 不会启用 benchmark；benchmark 还依赖 examples 和 Iceoryx。
上述命令从仓库根目录运行；`IBMW_BUILD_EXAMPLES=ON` 和
`IBMW_BUILD_ICEORYX_EXTENSION=ON` 是 benchmark gate 的依赖，DDS 行还需要
`IBMW_BUILD_DDS_EXTENSION=ON`，topology interop 的 DDS endpoint 还需要
`IBMW_BUILD_ROS2_TYPES=ON`。

```bash
cmake -S . -B ./build \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_BENCHMARKS=ON \
  -DIBMW_BUILD_ICEORYX_EXTENSION=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON
cmake --build ./build --parallel
cmake --install ./build --prefix ./build
bash ./scripts/run_bench_sweep.sh --reset-csv
```

可用 `IBMW_BUILD_DIR` 指定构建目录，用 `IBMW_BENCH_CSV` 指定结果文件。

## CSV 字段

```text
label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us
```

- `count == 0` 表示测量失败。
- 只有 sample、payload、callback mode、时长、预热、CPU affinity 和软件配置
  一致时，p99 才能直接比较。
- sync 与 async callback 是不同性能模式。async executor 投递增加 tail latency
  并不自动表示零拷贝发生 fallback。

## 可复现记录

每次用于决策的结果至少记录：

- 仓库版本或 release；
- 编译器、构建类型、启用的扩展、FastDDS/FastCDR 版本；
- CPU、NUMA、内存、内核、容器或虚拟化信息；
- 进程/IRQ affinity、调度策略、电源 governor；
- sample、payload、时长、预热、重复次数和 CSV 路径；
- loan mode、callback executor 等消息配置；
- 每轮原始结果与汇总方法。

至少运行三轮可比测量。解释延迟前，应先排查零接收、发现失败、异常值、
fallback 计数以及非预期分配或拷贝。

## 结果解释

- 功能收发成功不等于性能保证。
- 零拷贝应通过 API/backend 选择与运行标志确认，不能只凭延迟推断。
- payload 增大时共享内存通常更有价值，但调度、序列化、batching、发现与拓扑
  都可能主导具体工作负载。
- 热路径修改前，应在受控环境建立应用自己的 baseline 和 acceptance band；
  不要把另一台机器的数据作为回归门槛。

`topology_interop` 的 `ibmw_topology_dds_endpoint` 是 benchmark/direct DDS
实现：它直接使用 DDS transport driver、内部 typesupport 和扩展特定访问来
验证互操作拓扑。它不是 managed 应用应采用的架构示例；managed 应用仍应使用
公共 API，由配置选择动态扩展。

能力边界见[支持矩阵](../reference/support-matrix.md)，sample 说明见
[benchmark README](../../src/samples/cpp/transport_benchmark/README.md)。
