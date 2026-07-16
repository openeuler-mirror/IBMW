# Logger 示例

演示 IBMW 的 logging 系统：日志级别、格式化、按文件大小切割、同步写入与吞吐
基准测试。

## 运行

```bash
cd build

# 基础日志（所有级别）
bash start_examples_cpp_logger.sh

# 自定义格式
bash start_examples_cpp_logger_format.sh

# 日志切割（file backend）
bash start_examples_cpp_logger_rotate_file.sh

# 切割 + 同步写入
bash start_examples_cpp_logger_rotate_file_with_sync.sh

# 性能测试（吞吐）
bash start_examples_cpp_logger_bench.sh
```

## 期望输出（基础）

```
[Trace] This is a trace message
[Debug] This is a debug message
[Info]  This is an info message
[Warn]  This is a warning message
[Error] This is an error message
```

## 配置

| 配置文件 | 说明 |
|----------|------|
| `examples_cpp_logger_cfg.yaml` | Console backend，所有级别 |
| `examples_cpp_logger_format_cfg.yaml` | 自定义日志 pattern |
| `examples_cpp_logger_rotate_file_cfg.yaml` | File backend + 切割 |
| `examples_cpp_logger_rotate_file_with_sync_cfg.yaml` | 同步文件写入 |
| `examples_cpp_logger_bench_cfg.yaml` | 吞吐基准测试 |

## 涉及概念

- `IBMW_TRACE/DEBUG/INFO/WARN/ERROR()` —— 日志级别宏
- Console 与 file 两种日志 backend
- 按大小切割日志文件
- 通过 YAML 配置中的 `modules[].log_lvl` 设置单 module 级别

---

## ibmw-gen 方式

本 Demo 也可由 `ibmw-gen` 从
[`logging_demo.ibmw.yaml`](./logging_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 自动生成 | 你来写 |
|----------|--------|
| `CMakeLists.txt` —— 构建系统 | `logger_module.cc` —— 日志输出逻辑 |
| `cfg/logging_app.yaml` —— 基础 console 配置 | `logger_module.h` —— module 头文件 |
| `cfg/logging_app_format.yaml` —— 自定义 pattern 配置 | |
| `cfg/logging_app_rotate_file.yaml` —— 文件切割配置 | |
| `cfg/logging_app_rotate_file_with_sync.yaml` —— 同步写配置 | |
| `cfg/logging_app_specify_executor.yaml` —— 绑定 executor 的日志 | |
| `cfg/logger_module_pkg.yaml` —— Pkg 模式配置 | |
| `src/logging_app/main.cc`、`src/logger_module_pkg/pkg_main.cc` | |

### 关键 YAML 字段

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
    type: logger                    # 专用的 logger module 类型
    log_level: TRACE
    executor_name: work_executor

apps:
  - name: logging_app
    type: bare
    modules: [LoggerModule]
    config_variants:                # 一次性生成多个 YAML 配置
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

`config_variants` 特性允许从一份 app 定义生成多个 YAML 配置文件，每个用不同的
日志 backend 设置。这演示了 ibmw-gen 如何从单个 YAML 源产出一族配置。
