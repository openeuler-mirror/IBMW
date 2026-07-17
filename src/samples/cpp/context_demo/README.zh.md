# Context 示例

演示 IBMW Context API：在做资源隔离的前提下管理 module 生命周期。覆盖 executor
访问、logging、transport pub/sub、RPC —— 全部经由 Context 接口完成。

## 运行

```bash
cd build

# Executor context —— 1 到 10 计数的定时器循环
bash start_examples_cpp_context_executor.sh

# Logger context —— 5 个日志级别全跑一遍
bash start_examples_cpp_context_logger.sh

# Transport pub/sub —— publisher + subscriber（inline 回调）
bash start_examples_cpp_context_publisher.sh        # 终端 1
bash start_examples_cpp_context_subscriber_inline.sh # 终端 2（同进程）

# Transport pub/sub —— publisher + subscriber（在 executor 上）
bash start_examples_cpp_context_publisher.sh                 # 终端 1
bash start_examples_cpp_context_subscriber_on_executor.sh    # 终端 2

# RPC —— client + server（inline）
bash start_examples_cpp_context_rpc_client.sh         # 终端 1
bash start_examples_cpp_context_rpc_server_inline.sh  # 终端 2

# RPC —— client + server（在 executor 上）
bash start_examples_cpp_context_rpc_client.sh                  # 终端 1
bash start_examples_cpp_context_rpc_server_on_executor.sh      # 终端 2
```

## 期望输出

### Executor context
```
Timer loop: count=1
Timer loop: count=2
...
Timer loop: count=10
```

### Logger context
```
[Trace] Context logger trace
[Debug] Context logger debug
[Info]  Context logger info
[Warn]  Context logger warn
[Error] Context logger error
```

### Transport pub/sub
Publisher：
```
Published ExampleEventMsg: msg='hello', num=0
...
```
Subscriber：
```
Received ExampleEventMsg: msg='hello', num=0
...
```

### RPC
Client：
```
RPC response: "echo hello world"
...
```

## 配置

| 配置文件 | 说明 |
|----------|------|
| `examples_cpp_context_executor_cfg.yaml` | Executor context demo |
| `examples_cpp_context_logger_cfg.yaml` | Logger context demo |
| `examples_cpp_context_publisher_cfg.yaml` | Transport publisher |
| `examples_cpp_context_subscriber_inline_cfg.yaml` | Subscriber（inline） |
| `examples_cpp_context_subscriber_on_executor_cfg.yaml` | Subscriber（在 executor 上） |
| `examples_cpp_context_rpc_client_cfg.yaml` | RPC client |
| `examples_cpp_context_rpc_server_inline_cfg.yaml` | RPC server（inline） |
| `examples_cpp_context_rpc_server_on_executor_cfg.yaml` | RPC server（在 executor 上） |

## 涉及概念

- `ctx_ptr_->LetMe()` —— 建立 thread-local context 所有权
- `ctx_ptr_->GetExecutor()` —— 从 context 获取 executor
- `ctx_ptr_->GetLogger()` —— 从 context 获取 logger
- `IBMW_HL_INFO(logger, ...)` —— context 感知的日志
- 通过 context handle 做 transport pub/sub
- 通过 context handle 做 RPC client/server
- 基于协程（`co_await`）的 RPC 循环
- 通过 context ID 在 module 之间做资源隔离

## 关于 ibmw-gen

本 Demo 使用 **Context API**（`ibmw::context::Context`），是与其他 sample
所用的 Runtime API 不同的更高层抽象。`ibmw-gen` 代码生成器面向 Runtime API，
不会生成 Context API 风格的代码。要了解 ibmw-gen 在 Runtime API 项目中的用法，
参见 [ibmw-gen README](../../../tools/ibmw_gen/README.md)。
