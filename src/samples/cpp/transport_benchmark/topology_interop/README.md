# ROS2/IBMW topology benchmark

This benchmark runs four independent processes: `source -> relay -> sink A` and
`sink B`. The ROS2 and IBMW DDS executables use the same topics and message
contract:

* `/mw_perf/topology/raw` and `/mw_perf/topology/derived`
* `std_msgs/msg/UInt8MultiArray`, 4096 bytes, reliable, volatile, keep-last 32
* bytes 0..7: sequence; 8..15: origin monotonic send nanoseconds; 16..23:
  relay callback monotonic nanoseconds (zero on raw); 24..31: magic
  `0x544F504F4C4F4759`; remaining bytes are `0xA5`.

The default run is 100 Hz, 50 warmup messages, and 3000 measured messages.
Sinks validate every field, write latency CSV and status JSON, and write strict
`mw_perf.topology.v1` JSONL records followed by completion metadata. The JSONL
requires `MW_PERF_RUN_ID`, `MW_PERF_PRODUCER_PID`, and
`MW_PERF_PRODUCER_START_TIME_TICKS`; the runner supplies the run identity and
producer handoff when its process launcher supports environment inheritance.
The relay does not forward until both sinks are matched, and timestamps must
satisfy `origin_send_ns <= relay_receive_ns <= sink_receive_ns`.

Build targets are `ibmw_topology_ros2_endpoint` and
`ibmw_topology_dds_endpoint`. The DDS endpoint requires one of the four YAML
profiles in `config/`; domain 45 and the ROS2 `rt` topic prefix are intentional.

The DDS endpoint is a benchmark/direct DDS implementation. It links the DDS
transport driver and uses internal typesupport and extension-specific access to
exercise this topology. It is not a managed application architecture example;
managed applications use public APIs and select dynamically loaded extensions
through deployment configuration.

From the repository root, build the endpoint and run the local four-process lane:

```bash
cmake -S . -B ./build \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_BENCHMARKS=ON \
  -DIBMW_BUILD_ICEORYX_EXTENSION=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON
cmake --build ./build --target ibmw_topology_dds_endpoint --parallel
python3 ./src/samples/cpp/transport_benchmark/topology_interop/run_topology_matrix.py \
  --endpoint ./build/ibmw_topology_dds_endpoint \
  --config-dir ./src/samples/cpp/transport_benchmark/topology_interop/config
```

The required flags are `IBMW_BUILD_EXAMPLES=ON`, `IBMW_BUILD_BENCHMARKS=ON`,
`IBMW_BUILD_ICEORYX_EXTENSION=ON`, `IBMW_BUILD_DDS_EXTENSION=ON`, and
`IBMW_BUILD_ROS2_TYPES=ON`. The first three satisfy the benchmark gate; the last
two provide the DDS endpoint and ROS2-compatible type support. The runner writes
each result below `topology-artifacts/` and returns non-zero unless both sinks,
their completion records, and source/sink exit states validate.

Each invocation of the root-relative command above writes to a unique run
subdirectory. The launcher is bounded
and validates both sink artifacts, run IDs, and process exit states. For OHOS B,
configure with `IBMW_TARGET_ROS2_TIER=types`, an explicit
`IBMW_TARGET_ROS2_DEPS_PREFIX`, `IBMW_BUILD_ROS2_TYPES=ON`, and
`IBMW_BUILD_DDS_EXTENSION=ON`, then build `ibmw_topology_dds_endpoint`. OHOS
native ROS2 remains a separate project; the existing OHOS ABC runner is
pair-specific and is not reused for this four-process topology.

`run_ohos_topology_lane.sh` captures four-process `/proc` CPU boundaries from
five seconds into the run, midpoint PSS/RSS, and `top` snapshots. The matrix
runner alternates lane order and requires an explicit remote `valid=true`
result because `hdc shell` does not reliably propagate the remote exit code.
