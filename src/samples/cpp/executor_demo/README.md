# Executor Example

Demonstrates the IBMW executor framework: thread pools, timers, coroutines,
real-time scheduling, and coroutine loops.

## Run

```bash
cd build

# Basic executor (simple tasks + thread-safe counter)
bash start_examples_cpp_executor.sh

# Timer-based scheduling
bash start_examples_cpp_executor_timer.sh

# Coroutines
bash start_examples_cpp_executor_co.sh

# Coroutine loop
bash start_examples_cpp_executor_co_loop.sh

# Real-time executor (requires appropriate privileges)
bash start_examples_cpp_executor_real_time.sh
```

## Expected Output (basic)

```
Simple task executed.
Thread-safe counter: 10000
Time schedule loop iteration 1...
...
```

## Configurations

| Config | Description |
|--------|-------------|
| `examples_cpp_executor_cfg.yaml` | Basic executor with asio_thread pool |
| `examples_cpp_executor_timer_cfg.yaml` | Timer-based task scheduling |
| `examples_cpp_executor_co_cfg.yaml` | Coroutine executor |
| `examples_cpp_executor_co_loop_cfg.yaml` | Coroutine loop executor |
| `examples_cpp_executor_real_time_cfg.yaml` | Real-time executor with CPU binding |

## Concepts Demonstrated

- `core_.GetExecutor("name")` -- get executor handle
- `executor.Execute(task)` -- submit task to executor
- `executor.ExecuteAt(tp, task)` -- schedule task at time point
- Thread-safe task execution across thread pools
- Coroutine-based scheduling (`co_await`)

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`executor_demo.ibmw.yaml`](./executor_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system | `executor_module.cc` -- dispatch/strand/timed logic |
| `cfg/executor_app.yaml` -- 3 executors (thread_pool + strand) | `executor_co_module.cc` -- coroutine logic |
| `cfg/executor_co_app.yaml` -- coroutine executor config | `executor_co_loop_module.cc` -- periodic loop |
| `cfg/executor_co_loop_app.yaml` -- single executor | `timer_module.cc` -- CreateTimer/Reset/Cancel |
| `cfg/executor_timer_app.yaml` -- timer executor config | `real_time_module.cc` -- SCHED_FIFO/RR setup |
| `cfg/executor_real_time_app.yaml` -- RT executors with CPU affinity | Module `.h` headers |
| `cfg/*_pkg.yaml` -- 5 Pkg-mode configs | |
| `src/*/main.cc`, `src/*_pkg/pkg_main.cc` -- 5 apps + 5 pkg loaders | |

### Generate, build, and run

```bash
# Generate
python3 <ibmw-gen>/ibmw_gen.py executor_demo.ibmw.yaml -o executor_demo_gen

# Build
mkdir -p executor_demo_gen/build && cd executor_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# Run any of the 5 apps:
./executor_app cfg/executor_app.yaml
./executor_co_app cfg/executor_co_app.yaml
./executor_co_loop_app cfg/executor_co_loop_app.yaml
./executor_timer_app cfg/executor_timer_app.yaml
./executor_real_time_app cfg/executor_real_time_app.yaml
```

### Key YAML features

```yaml
# Per-app executor definitions (different executors per app)
apps:
  - name: executor_app
    type: bare
    modules: [ExecutorModule]
    executors:                            # Per-app executor override
      - name: work_executor
        type: thread_pool
        options: { thread_num: 2 }
      - name: thread_safe_executor
        type: strand
        options: { bind_thread_pool_executor_name: work_executor }

  - name: executor_real_time_app
    type: bare
    modules: [RealTimeModule]
    executors:
      - name: sched_fifo_thread
        type: thread_pool
        options:
          thread_sched_policy: "SCHED_FIFO:80"  # Real-time scheduling
          thread_bind_cpu: [0, 1]               # CPU affinity
```

This is the most complex ibmw-gen demo, demonstrating **per-app executor
definitions**. Unlike project-level executors shared by all apps, each app
defines its own executor set -- ibmw-gen generates separate YAML configs with
the correct executor sections for each.

All 5 modules are `bare` type (no transport). The user writes the actual
executor interaction code (dispatch, coroutines, timers, real-time policies)
inside the generated module scaffolds.
