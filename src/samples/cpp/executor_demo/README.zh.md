# Executor 示例

演示 IBMW executor 框架：thread pool、timer、coroutine、实时调度，以及
coroutine loop。

## 运行

```bash
cd build

# 基础 executor（简单任务 + 线程安全计数器）
bash start_examples_cpp_executor.sh

# 基于 timer 的调度
bash start_examples_cpp_executor_timer.sh

# Coroutine
bash start_examples_cpp_executor_co.sh

# Coroutine loop
bash start_examples_cpp_executor_co_loop.sh

# 实时 executor（需要相应权限）
bash start_examples_cpp_executor_real_time.sh
```

## 预期输出（基础）

```
Simple task executed.
Thread-safe counter: 10000
Time schedule loop iteration 1...
...
```

## 配置

| 配置文件 | 说明 |
|--------|-------------|
| `examples_cpp_executor_cfg.yaml` | 使用 asio_thread 池的基础 executor |
| `examples_cpp_executor_timer_cfg.yaml` | 基于 timer 的任务调度 |
| `examples_cpp_executor_co_cfg.yaml` | Coroutine executor |
| `examples_cpp_executor_co_loop_cfg.yaml` | Coroutine loop executor |
| `examples_cpp_executor_real_time_cfg.yaml` | 带 CPU 绑定的实时 executor |

## 涉及概念

- `core_.GetExecutor("name")` —— 获取 executor 句柄
- `executor.Execute(task)` —— 向 executor 提交任务
- `executor.ExecuteAt(tp, task)` —— 在指定时间点调度任务
- 跨线程池的线程安全任务执行
- 基于 coroutine 的调度（`co_await`）

---

## ibmw-gen 方式

本 demo 也可以通过 `ibmw-gen` 从
[`executor_demo.ibmw.yaml`](./executor_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 生成（自动） | 你自己写 |
|-------------------|-----------|
| `CMakeLists.txt` —— 构建系统 | `executor_module.cc` —— dispatch/strand/timed 逻辑 |
| `cfg/executor_app.yaml` —— 3 个 executor（thread_pool + strand） | `executor_co_module.cc` —— coroutine 逻辑 |
| `cfg/executor_co_app.yaml` —— coroutine executor 配置 | `executor_co_loop_module.cc` —— 周期性 loop |
| `cfg/executor_co_loop_app.yaml` —— 单 executor | `timer_module.cc` —— CreateTimer/Reset/Cancel |
| `cfg/executor_timer_app.yaml` —— timer executor 配置 | `real_time_module.cc` —— SCHED_FIFO/RR 设置 |
| `cfg/executor_real_time_app.yaml` —— 带 CPU 亲和性的 RT executor | Module `.h` 头文件 |
| `cfg/*_pkg.yaml` —— 5 份 Pkg 模式配置 | |
| `src/*/main.cc`、`src/*_pkg/pkg_main.cc` —— 5 个 app + 5 个 pkg 加载器 | |

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py executor_demo.ibmw.yaml -o executor_demo_gen

# 构建
mkdir -p executor_demo_gen/build && cd executor_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行 5 个 app 中的任意一个：
./executor_app cfg/executor_app.yaml
./executor_co_app cfg/executor_co_app.yaml
./executor_co_loop_app cfg/executor_co_loop_app.yaml
./executor_timer_app cfg/executor_timer_app.yaml
./executor_real_time_app cfg/executor_real_time_app.yaml
```

### 关键 YAML 特性

```yaml
# 每个 app 单独定义 executor（不同 app 用不同 executor）
apps:
  - name: executor_app
    type: bare
    modules: [ExecutorModule]
    executors:                            # 按 app 覆盖 executor
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
          thread_sched_policy: "SCHED_FIFO:80"  # 实时调度
          thread_bind_cpu: [0, 1]               # CPU 亲和性
```

这是最复杂的 ibmw-gen demo，演示了 **per-app executor 定义**。与所有 app 共享
的 project 级 executor 不同，这里每个 app 都定义自己的 executor 集合 ——
ibmw-gen 会为每个 app 生成带正确 executor 段的独立 YAML 配置。

5 个 module 全部是 `bare` 类型（不带 transport）。用户在生成的 module 脚手架内
编写实际的 executor 交互代码（dispatch、coroutine、timer、实时策略）。
