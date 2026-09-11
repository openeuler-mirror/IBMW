# Transport Benchmark Methodology

IBMW provides a common latency harness for comparing shared-memory, DDS, and TCP
paths with the same payload registry and percentile calculation. Published
numbers are environment-specific; reproduce them on the intended platform before
making an application decision.

## Build and run

The `full` preset does not enable benchmarks; benchmarks also depend on examples
and Iceoryx. Run the commands below from the repository root. The benchmark gate
requires `IBMW_BUILD_EXAMPLES=ON` and `IBMW_BUILD_ICEORYX_EXTENSION=ON`; DDS
benchmarks additionally require `IBMW_BUILD_DDS_EXTENSION=ON`, and the DDS
endpoint for topology interop additionally requires `IBMW_BUILD_ROS2_TYPES=ON`.

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

The default sweep covers the maintained payload sizes and transport samples under
`src/samples/cpp/transport_benchmark/`. Set `IBMW_BUILD_DIR` for an out-of-tree
build and `IBMW_BENCH_CSV` for a custom result path.

## Result schema

```text
label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us
```

- `count == 0` is a failed measurement.
- Compare p99 only between runs with the same sample, payload, callback mode,
  duration, warm-up, CPU affinity, and software configuration.
- Sync and async callback modes are separate performance contracts. Async
  executor handoff can increase tail latency without implying a zero-copy
  fallback.

## Reproducibility record

For every result used in a decision, record:

- repository version or release;
- compiler, build type, enabled extensions, FastDDS/FastCDR versions;
- CPU, memory/NUMA topology, kernel, container/virtualization details;
- process and IRQ affinity, scheduler policy, power governor;
- sample, payload size, duration, warm-up, repetitions, and CSV path;
- messaging configuration, including loan mode and callback executor;
- raw per-run results plus the aggregation method.

Run at least three comparable repetitions. Investigate zero receive counts,
outliers, discovery failures, fallback counters, and unexpected allocation/copy
markers before interpreting latency.

## Interpreting results

- Functional delivery does not establish a performance guarantee.
- Zero-copy must be confirmed from the selected API/backend and runtime markers,
  not inferred only from latency.
- Shared-memory paths generally become more valuable as payload size grows, but
  scheduling, serialization, batching, discovery, and topology can dominate a
  specific workload.
- Establish an application baseline from a controlled environment and define an
  acceptance band before changing hot-path code. Do not use results from another
  machine as a regression threshold.

`topology_interop`'s `ibmw_topology_dds_endpoint` is a benchmark/direct DDS
implementation. It directly uses the DDS transport driver, internal typesupport,
and extension-specific access to validate interop topologies. It is not an
architectural example for managed applications; managed applications should
continue to use the public API, with dynamic extensions selected through
configuration.

Capability boundaries are summarized in the
[Support Matrix](../reference/support-matrix.md). Source-level sample details are
in the [benchmark README](../../src/samples/cpp/transport_benchmark/README.md).
