# transport_benchmark

End-to-end latency / throughput benchmark suite that powers the comparison
methodology in [`docs/guides/transport-benchmark.md`](../../../../docs/guides/transport-benchmark.md)
(English) and [`docs/guides/transport-benchmark.zh.md`](../../../../docs/guides/transport-benchmark.zh.md)
(Chinese).

Each sub-sample is a self-contained publisher + subscriber pair (pkg-mode +
app-mode) measuring one transport / API combination under the same X-macro
payload registry, so latency numbers are directly comparable across rows.

## Build gate

The whole subtree is gated by `IBMW_BUILD_BENCHMARKS=ON` (declared in the
top-level `CMakeLists.txt`). The `full` preset enables it; `minimal` does
not. Iceoryx-based samples additionally require
`IBMW_BUILD_ICEORYX_EXTENSION=ON`.

```bash
cmake --preset full -DIBMW_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
```

## Benchmark sample set

The source tree contains eight benchmark subdirectories. Seven are part of the
canonical `run_bench_sweep.sh` regression matrix; `iox_loan_v2/` is a focused
public-API comparison against the lower-level Iceoryx loan path and is kept
outside the default 6×7 sweep.

| Subdir | Backend | API surface | What it measures |
|--------|---------|-------------|------------------|
| `iox_loaned_pod/` | Iceoryx | extension-only POD loan | Best-case zero-copy: POD payload, no serialization, direct SHM chunk |
| `iox_loaned_ctx/` | Iceoryx | standard receive stack | Zero-copy via the ctx-meta path through the standard receive pipeline |
| `iox_send/` | Iceoryx | standard `Send()` (with serialization) | Iceoryx baseline with full CDR (de)serialization round-trip |
| `dds_loaned/` | FastDDS | `LoanedMessage<T>` | DDS DataSharing/SHM zero-copy path |
| `dds_send_shm/` | FastDDS | standard `Send()` over SHM transport | DDS over SHM with serialization, no DataSharing |
| `dds_send_tcp/` | FastDDS | standard `Send()` over TCP-only transport | DDS pure-TCP baseline (no SHM optimization) |
| `net_send_tcp/` | net_extension | standard `Send()` over native TCP | `libibmw_net_transport.so` raw TCP — control sample |
| `iox_loan_v2/` | Iceoryx | **public user-level loan facade** (`LoanedSample<T>` + `SenderRef::Loan/PublishLoaned`) | The same loan path encapsulated for end-user code; wire format byte-equal to `iox_loaned_pod`, only the publish-side API surface differs |

The `iox_loan_v2_gen/` directory you may see locally is an **untracked
build artifact** produced by ibmw-gen during build-matrix runs — it
is not part of the source tree.

## Shared infrastructure

| Subdir | Role |
|--------|------|
| `bench_payload/` | X-macro driven payload registry: `bench_payload.h`, `bench_send_payloads.h`, `bench_plain_payloads.h` define the standard 6-size matrix (1KiB / 4KiB / 16KiB / 64KiB / 256KiB / 1MiB) and register them as DDS-typed templates so every sample can pick a payload by index |
| `bench_common/` | `bench_options.h` — shared CLI option parsing + statistics aggregator (p50 / p90 / p99 / p99.9). Single source of truth for histogram math |
| `publish_tap/` | In-process dispatcher microbenchmark for no tap, registered idle tap, and a bounded owned-copy recording-style tap; reports repeated p99 and per-Send allocation counts without a fixed cross-machine threshold |

The shared payload and statistics libraries are why bench numbers are
comparable across rows: every sample loads the same payload sizes and computes
percentiles with the same estimator.

### Publish-tap hot-path evidence

With `IBMW_BUILD_BENCHMARKS=ON`, build and run the focused dispatcher
benchmark from the build root:

```bash
cmake --build build --target ibmw_publish_tap_benchmark -j2
./ibmw_publish_tap_benchmark --iterations 2000 --warmup 200 --repeats 3 \
  --payload-bytes 4096 --queue-depth 256
```

The CSV output has one row per repeat and mode (`no_tap`, `idle_tap`, and
`owned_copy_tap`) and includes p50/p90/p99/p99.9/max latency, allocation counts
for the measured `Send()` loop, callback counts, and bounded-queue overwrites.
The third mode models the production runtime tap's synchronous owned-copy
boundary; it is not a substitute for the runtime socket/SQLite end-to-end
validators. Results are host-local evidence and intentionally have no fixed
cross-machine acceptance threshold.

## Run

The canonical entry point is the sweep script, which handles per-sample
RouDi/SHM cleanup, the full 6×7 size×backend matrix, CSV emission, and the
process-management idiom (no `pkill`):

```bash
set +u; source /opt/ros/humble/setup.bash; set -u
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

bash scripts/run_bench_sweep.sh --reset-csv
```

The sweep writes the configured CSV and per-sample logs under
`${TMPDIR:-/tmp}`. Preserve raw outputs with the environment record described in
the public methodology.

## Per-sample documentation

The detailed reproduction recipe (config layout, RouDi setup, CSV column
semantics, p99 reading guide) lives in
[`docs/guides/transport-benchmark.md`](../../../../docs/guides/transport-benchmark.md).
This README is the source-tree index; the user-facing tutorial is the
benchmark doc.

## Performance regression policy

Establish a controlled, application-specific baseline with repeated runs before
changing hot-path code. Investigate results outside the predeclared acceptance
band using the public methodology; do not use measurements from another machine
as a universal regression threshold.
