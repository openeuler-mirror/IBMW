# A/B/C 4 KiB interop benchmark

This is a deliberately small endpoint and does not change existing business
code or ABI.

## A: native ROS 2 Humble

Built only with `IBMW_BUILD_BENCHMARKS=ON` and `IBMW_BUILD_WITH_ROS2=ON` when
`rclcpp` and `std_msgs` are discoverable. Run two processes (source a ROS 2
Humble environment first):

```sh
ibmw_abc_ros2_endpoint --role sub --csv /tmp/abc.csv --message-count 10000
ibmw_abc_ros2_endpoint --role pub --message-count 10000 --hz 1000
```

The topic is `/mw_perf/payload_4kb`; the type is
`std_msgs/msg/UInt8MultiArray`; QoS is reliable, volatile, keep-last 8192.
Each 4096-byte payload starts with packed host values `{uint64 send_ns, uint64 seq}`;
the remaining bytes are either all zero or all `0xA5` and are fully validated.
Status consistently reports attempted/received/unique counts, missing,
duplicate, out-of-order, invalid-payload, invalid-timestamp, and validity.
CSV uses `LatencyStats` and monotonic clock timestamps.

For producer-owned managed collection, use `--jsonl PATH`. The subscriber then
reads collector-injected run identity, buffers measured unique valid records,
stays alive after the target count, and writes the strict completion trailer on
collector SIGTERM. From the repository root, `a_lane_managed.json` and
`run_a_managed.sh` provide this benchmark's managed layout: the shell starts the
publisher as a descendant and execs the subscriber as collector leader. The
JSON is a repository-root example with a sample build path, not a generally
relocatable collector configuration; replace the endpoint build path before use.

## B: IBMW DDS ROS 2-compatible

When `ROS2_TYPES+DDS_EXTENSION` are enabled, `ibmw_abc_dds_endpoint` is a real
two-process IBMW endpoint using `RegisterSendType`, `Send`, and `OnMessage` with
`std_msgs::msg::UInt8MultiArray` (not a String surrogate). From the build root:

```sh
./ibmw_abc_dds_endpoint --role sub --config cfg/abc_interop/subscriber.yaml \
  --csv /tmp/abc.csv --message-count 10000
./ibmw_abc_dds_endpoint --role pub --config cfg/abc_interop/publisher.yaml \
  --message-count 10000 --hz 1000
```

Both processes stop after the requested fixed count; the subscriber prints and
appends the same `LatencyStats` CSV schema as A. Both
configs set `ros2_compatible: true`, reliable/volatile keep-last 8192, and map
the fixed `/mw_perf/payload_4kb` topic through ROS 2's `rt` DDS prefix. The
payload and its first 16 bytes are byte-for-byte identical to A. B labels rows
`abc_dds_4KB`. Since `Send` has no result, publisher counts are explicitly
named `attempted_total` and `attempted_measured`, not sent counts.

## C: iceoryx ordinary `Send()`

`iox_send_publisher_app` and `iox_send_subscriber_app` are the existing
ordinary `Send()` benchmark path and are reused when
`IBMW_BUILD_ICEORYX_EXTENSION=ON`. Use their copied `iox_send` configuration
with `payload_bytes: 4096`, matching `hz`, `warmup_messages`, and fixed
`message_count` (fixed-count publishing sends warmup plus measured messages).
The supplied 50 warmup and 3000 measured contract sends 3050 total and
requires 3000 valid measured receives; incomplete/invalid C runs do not append
CSV.
The C endpoint retains the existing IBMW YAML/process CLI rather than claiming
a new argument ABI: use the equivalent mappings `--hz` -> `hz`,
`--warmup-messages` -> `warmup_messages`, `--message-count` ->
`message_count`, `--csv` -> subscriber `csv_path`, and `--topic` -> `topic`.
`start_delay_ms` supplies a bounded readiness delay because C has no DDS
reliable discovery/match handshake. The supplied C configuration uses
`/mw_perf/payload_4kb`. Start RouDi with
`start_iox_send.sh`. This
path intentionally does **not** use a
loaned API. Capability warning: iceoryx provides shared-memory transport, but
there is no DDS protocol-level reliability/discovery semantic; results must
not be described as DDS reliable delivery.

## Gates and smoke

The manually assembled OHOS test bundle must include executable
`validate_ohos_artifact.sh` beside `run_ohos_lane.sh` at the bundle root.

The Python runner requires exactly one `LatencyStats` CSV row with the fixed
schema, lane label, finite numeric values, and `count=3000`. Every status must
be a complete JSON object with boolean `valid`, integer attempted/sent,
received/unique and all five error counters; missing, null, or non-object
statuses are rejected. C additionally requires its independent publisher-only
status schema: boolean `completion`/`valid` and nonnegative integer
`attempted_total`/`attempted_measured`; it does not fabricate subscriber fields.
Every artifact used by a gate (CSV and subscriber or publisher status) must have
`mtime_ns >= run_started_ns`. A/B processes must
exit zero. C processes may exit zero or from the runner's intentional SIGINT;
any timeout/kill remains invalid. Results record
`process_exit_codes`, `validation_errors`, and `validation_warnings`. A/B's
fixed readiness sleep is retained; the current runner does not generate a
dedicated warning for it. C requires a fresh
structured publisher status with `valid=true`, `completion=true`, and exactly
3000 attempted measured messages. A missing, stale, malformed, incomplete, or
wrong-count publisher artifact fails the run; publisher logs are never a
completion fallback. The publisher writes this artifact atomically on natural
or early loop termination, with early termination marked incomplete/invalid.
The runner waits up to 30 seconds for that final status before shutdown. Matrix
lanes have a 120-second subprocess bound; summaries include `valid_runs` and
`invalid_runs`, and `--blocks` must be at least one.

The parent benchmark gate remains `IBMW_BUILD_BENCHMARKS`; A additionally
requires `WITH_ROS2` (`IBMW_BUILD_WITH_ROS2`), B requires `IBMW_BUILD_ROS2_TYPES`
and `IBMW_BUILD_DDS_EXTENSION`, and C requires `IBMW_BUILD_ICEORYX_EXTENSION`.
`cmake --build <build> --target ibmw_abc_smoke` checks target wiring without
starting middleware. `extension_v2_fixture.cc` is untouched.

For lane C, CMake writes the resolved source-built or package-provided RouDi
target location to `<build>/ibmw_abc_roudi_path.txt`. `run_block.py` and
`run_matrix.py` use it automatically. Use `--roudi /path/to/iox-roudi` (or set
`IBMW_ROUDI_BIN`) to override that location explicitly.

`run_matrix.py --comparison-plan PLAN.json` also emits a formal
`mw_perf.comparison-plan.v1` and a bridge `manifest.json`/`validation.json` in
each lane directory. Supply comparison declarations with `--contract`,
`--profile`, `--workload`, and `--resource-scope`; JSON values are accepted.
The fixed lane sequence is accompanied by `--order-policy` and `--order-seed`.
This bridge does not run a collector lifecycle or claim resource/provenance
evidence: those capabilities and omitted declarations are marked unsupported,
and its validation is intentionally invalid for comparison. Formal compare
therefore accepts the plan shape but fails closed until each lane directory is
replaced by a genuinely collected and validated run carrying matching metadata.

Each OHOS lane also records CPU evidence: `cpu-uptime-start.txt`,
`cpu-uptime-end.txt`, `pub-proc-stat-start.txt`, `sub-proc-stat-start.txt`,
`pub-proc-stat-end.txt`, `sub-proc-stat-end.txt`, `top-mid.txt`, `top-end.txt`,
and `cpu-summary.json`. The summary uses `/proc/<pid>/stat` fields 14 and 15
(user and system CPU ticks), `/proc/uptime`, and `CLK_TCK=100`:
`cpu_pct = ((end_utime + end_stime) - (start_utime + start_stime)) /
(CLK_TCK * elapsed_seconds) * 100`. `pair_cpu_pct` is the sum of publisher and
subscriber CPU percentages, so overlapping CPU use is intentionally retained.
After the subscriber starts and initial proc setup, both endpoint processes must
remain alive for a fixed five-second startup settle; this settle is excluded from
CPU measurement. The CPU interval then uses
`floor((warmup_messages + message_count) / hz) - 5 - 5` seconds, with a minimum
of one second. The final five-second margin is reserved for publisher-first
discovery and startup; for the 100 Hz, 50-warmup, 3000-measured contract the
post-settle CPU window is 20 seconds. Its start uptime and proc-stat snapshots
are captured after the settle, and its end snapshots are captured together
before publisher shutdown.
An endpoint exiting before that boundary invalidates the CPU summary and run;
later cleanup never replaces the boundary files. The midpoint and end `top`
snapshots remain supporting evidence and are validity-gated: each must be
nonempty and contain both endpoint PIDs. `cpu-summary.json` uses the same
uptime/proc-stat start and end boundaries as this fixed window.
