# Logger Example

Demonstrates the IBMW logging system: log levels, formatting, file rotation,
synchronous writes, and performance benchmarks.

## Run

```bash
cd build

# Basic logging (all levels)
bash start_examples_cpp_logger.sh

# Custom formatting
bash start_examples_cpp_logger_format.sh

# Log rotation (file backend)
bash start_examples_cpp_logger_rotate_file.sh

# Log rotation with sync writes
bash start_examples_cpp_logger_rotate_file_with_sync.sh

# Benchmark (throughput test)
bash start_examples_cpp_logger_bench.sh
```

## Expected Output (basic)

```
[Trace] This is a trace message
[Debug] This is a debug message
[Info]  This is an info message
[Warn]  This is a warning message
[Error] This is an error message
```

## Configurations

| Config | Description |
|--------|-------------|
| `examples_cpp_logger_cfg.yaml` | Console backend, all levels |
| `examples_cpp_logger_format_cfg.yaml` | Custom log pattern |
| `examples_cpp_logger_rotate_file_cfg.yaml` | File backend with rotation |
| `examples_cpp_logger_rotate_file_with_sync_cfg.yaml` | Sync file writes |
| `examples_cpp_logger_bench_cfg.yaml` | Throughput benchmark |

## Concepts Demonstrated

- `IBMW_TRACE/DEBUG/INFO/WARN/ERROR()` -- log level macros
- Console and file log backends
- Log file rotation by size
- Per-module log levels via `modules[].log_lvl` in YAML config

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`logging_demo.ibmw.yaml`](./logging_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system | `logger_module.cc` -- log output logic |
| `cfg/logging_app.yaml` -- basic console config | `logger_module.h` -- module header |
| `cfg/logging_app_format.yaml` -- custom pattern config | |
| `cfg/logging_app_rotate_file.yaml` -- file rotation config | |
| `cfg/logging_app_rotate_file_with_sync.yaml` -- sync write config | |
| `cfg/logging_app_specify_executor.yaml` -- executor-bound logging | |
| `cfg/logger_module_pkg.yaml` -- Pkg-mode config | |
| `src/logging_app/main.cc`, `src/logger_module_pkg/pkg_main.cc` | |

### Key YAML features

```yaml
logging:
  core_level: INFO
  backends:
    - type: console

executors:
  - name: sync_timer_executor
    type: thread_pool
  - name: log_executor
    type: thread_pool

modules:
  - name: LoggerModule
    type: logger                    # Special logger module type
    log_level: TRACE
    executor_name: work_executor

apps:
  - name: logging_app
    type: bare
    modules: [LoggerModule]
    config_variants:                # Generate multiple YAML configs
      - suffix: format
        logging:
          backends:
            - type: console
              options:
                pattern: "[%c.%f][%A][%l][%t][%n][%G] %v"
      - suffix: rotate_file
        logging:
          backends:
            - type: rotate_file
              options: { path: ./log, max_file_size_m: 4 }
```

The `config_variants` feature generates multiple YAML config files from a single
app definition, each with different logging backend settings. This demonstrates
how ibmw-gen can produce a family of configs from one YAML source.
