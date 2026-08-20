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
collector SIGTERM. `a_lane_managed.json` and `run_a_managed.sh` provide a real
managed layout where the shell starts the publisher as a descendant and execs
the subscriber as collector leader; replace the endpoint build path before use.

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
fixed readiness sleep is retained and reported as a warning. C requires a fresh
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
