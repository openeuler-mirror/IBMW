# extension_smoke_test

Configuration-only smoke test bundle for IBMW extensions. There is **no**
hand-written C++ in this directory — the test harness runs every committed
extension config against the stock `ibmw run` driver under a short timeout
and asserts that each scenario reaches a steady-running state without
crashing.

## What this sample proves

For each of the 15 YAML configs in `cfg/`, `run_all_tests.sh` invokes:

```
timeout -s KILL 8s ./bin/ibmw run cfg/ext_<name>_cfg.yaml
```

and treats exit codes `0`, `124` (TERM), `130` (SIGINT), and `137` (SIGKILL)
as **PASS** — IBMW demos do not self-terminate, so reaching the timeout
cleanly is the success criterion. Any other exit code is a regression in the
named extension's load / configure / run path.

## Configurations under test

| Cfg file | Extension exercised |
|----------|---------------------|
| `ext_echo_driver_cfg.yaml` | `libibmw_echo_driver.so` (RPC echo) |
| `ext_grpc_transport_cfg.yaml` | `libibmw_grpc_transport.so` (transport) |
| `ext_grpc_rpc_cfg.yaml` | `libibmw_grpc_transport.so` (RPC client/server) |
| `ext_log_control_cfg.yaml` | `libibmw_log_control.so` (runtime log-level RPC) |
| `ext_mqtt_transport_cfg.yaml` | `libibmw_mqtt_transport.so` |
| `ext_net_transport_cfg.yaml` | `libibmw_net_transport.so` (TCP/UDP pub/sub) |
| `ext_net_http_pubsub_cfg.yaml` | `libibmw_net_transport.so` (HTTP transport variant) |
| `ext_parallel_executor_cfg.yaml` | TBB-backed `parallel` executor |
| `ext_parameter_service_cfg.yaml` | `libibmw_parameter_service.so` |
| `ext_proxy_driver_cfg.yaml` | `libibmw_proxy_driver.so` (topic forwarding) |
| `ext_record_playback_cfg.yaml` | `libibmw_record_playback.so` |
| `ext_service_introspection_cfg.yaml` | `libibmw_service_introspection.so` |
| `ext_time_manipulator_cfg.yaml` | `libibmw_time_manipulator.so` |
| `ext_topic_logger_cfg.yaml` | `libibmw_topic_logger.so` |
| `ext_zenoh_transport_cfg.yaml` | `libibmw_zenoh_transport.so` |

## Layout

| Path | Role |
|------|------|
| `cfg/ext_*_cfg.yaml` | One config per extension — minimal valid config that loads the extension and (for transport/RPC backends) wires a publisher+subscriber or server+client pair |
| `run_all_tests.sh` | Driver script for the smoke-test pass. Also runs all standalone-app demos and all hand-written pkg-mode demos in the same pass |

The directory has no `CMakeLists.txt` — `run_all_tests.sh` is staged into
`build/` (and into `/tmp/ibmw_regress/stage/` by `scripts/stage_smoke.sh`)
through the parent `samples/cpp` install logic.

## Run

The driver script is normally invoked from the staged smoke directory, not
from `build/` directly:

```bash
bash scripts/stage_smoke.sh
cd /tmp/ibmw_regress/stage
LD_LIBRARY_PATH=$PWD/lib:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH ./run_all_tests.sh
```

CMake generates `cli-stage.yaml` from the enabled CLI providers. The staging
helper copies it to `stage/cli.yaml`, where provider paths are relative to
`stage/lib`. Both `stage/ibmw` and `stage/bin/ibmw` use this manifest, including
after moving the staged directory. Reconfigure the build before staging if
`cli-stage.yaml` is missing.

Useful flags:

```bash
./run_all_tests.sh --quick       # Skip DDS/ROS2 tests
./run_all_tests.sh --ext-only    # Run only the 15 extension cfg tests
```

## Where it shows up in the verification suite

- The smoke run executes all 15 `ext_*_cfg.yaml` configs plus standalone-app
  and pkg-mode demos in a single pass. It is the canonical regression net
  for the extension load path. Some configs are guarded by missing optional
  dependencies (e.g., MQTT broker, gRPC peer) and are skipped without
  counting as failures.

> **Note on the timeout-as-PASS convention.** `ibmw run` is a long-running
> daemon — it has no graceful self-shutdown for these configs. The 8-second
> timeout window is long enough for every subsystem to finish init and
> publish at least one message; reaching the timeout therefore witnesses
> "extension loaded + ran without crashing".
