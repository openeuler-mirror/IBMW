# engine_smoke_test

Comprehensive smoke test for the IBMW core engine (`ibmw::runtime::core::Engine`),
covering R10 (topological subsystem dependency graph + init/shutdown ordering +
init rollback) and R15 (polymorphic initialization report).

This sample is **self-checking** — it exits with code `0` only when all 8
internal test cases pass.

## What this sample proves

| # | Test case | Verifies |
|---|-----------|----------|
| 1 | No-config lifecycle | `Initialize → AsyncStart → Shutdown` works without a YAML file |
| 2 | Topological init order | The 11 subsystems initialize in a valid topological order against the static dependency graph (Config → Extensions/MainLoop/Watchdog → Executors → Logging → Memory/RPC/Transport/Parameters → Modules) |
| 3 | Shutdown reverse order | Subsystem `stopped` events appear in the exact reverse of their `initialized` order (timestamp-sorted, since stdout vs stderr buffering may permute log lines) |
| 4 | R15 init report | `--- System ---`, `--- Configuration ---`, `--- Extensions ---`, `--- Executors ---`, `--- Logging ---`, `--- RPC ---`, `--- Transport ---`, `--- Modules ---` sections appear in the report, in order; `Memory` and `Parameters` (empty reports) are omitted |
| 5 | Init rollback | A YAML referencing a nonexistent `.so` triggers controlled rollback: the engine throws, ends in `Phase::kStopped`, and a `rolling back` log line plus at least one `stopped` event are emitted |
| 6 | Phase transitions | `kConfigure → kInitialize → kRunning → kStopped` |
| 7 | Config-based lifecycle | A YAML with a custom `thread_pool` executor + console logger ends up reflected in the R15 report |
| 8 | Sequential lifecycles | 3 back-to-back `Engine` instances do not leak resources or deadlock |

## Layout

| Path | Role |
|------|------|
| `standalone/engine_smoke_test_app/main.cc` | All 8 test cases + minimal `OutputCapture` helper |
| `standalone/engine_smoke_test_app/CMakeLists.txt` | Single-target executable |
| `CMakeLists.txt` | Wires the app into `samples/cpp/<namespace>_engine_smoke_test_build_all` |

The app links only against `ibmw::core::engine` — no transport extensions, no
DDS/Iceoryx/ROS2 headers — which is why the test is included in **every**
preset (minimal, dev, full) and runs on hosts without `/opt/ros/humble`.

## Build

Built unconditionally as part of the `samples` tree.

```bash
cmake --preset full
cmake --build build -j$(nproc) --target engine_smoke_test_app
```

## Run

```bash
cd build
./engine_smoke_test_app
```

The expected output ends with:

```
========================================
Results: 8 passed, 0 failed
========================================
```

Non-zero exit means a regression in the core engine's R10/R15 contracts and
must be root-caused before further refactor or release sign-off.

## Where it shows up in the verification suite

- Invoked from the host smoke-test driver `run_all_tests.sh` after staging
  the install tree. Failure halts the smoke run.
- Not part of `ctest` (the tests live inside the app binary itself, not as
  individual CTest entries).
