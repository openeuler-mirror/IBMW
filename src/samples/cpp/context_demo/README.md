# Context Example

Demonstrates the IBMW Context API for module lifecycle management with resource
isolation. Covers executor access, logging, transport pub/sub, and RPC -- all
through the Context interface.

## Run

```bash
cd build

# Executor context -- timer loop counting 1-10
bash start_examples_cpp_context_executor.sh

# Logger context -- all 5 log levels
bash start_examples_cpp_context_logger.sh

# Transport pub/sub -- publisher + subscriber (inline callback)
bash start_examples_cpp_context_publisher.sh        # Terminal 1
bash start_examples_cpp_context_subscriber_inline.sh # Terminal 2 (same process)

# Transport pub/sub -- publisher + subscriber (on executor)
bash start_examples_cpp_context_publisher.sh                 # Terminal 1
bash start_examples_cpp_context_subscriber_on_executor.sh    # Terminal 2

# RPC -- client + server (inline)
bash start_examples_cpp_context_rpc_client.sh         # Terminal 1
bash start_examples_cpp_context_rpc_server_inline.sh  # Terminal 2

# RPC -- client + server (on executor)
bash start_examples_cpp_context_rpc_client.sh                  # Terminal 1
bash start_examples_cpp_context_rpc_server_on_executor.sh      # Terminal 2
```

## Expected Output

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
Publisher:
```
Published ExampleEventMsg: msg='hello', num=0
...
```
Subscriber:
```
Received ExampleEventMsg: msg='hello', num=0
...
```

### RPC
Client:
```
RPC response: "echo hello world"
...
```

## Configurations

| Config | Description |
|--------|-------------|
| `examples_cpp_context_executor_cfg.yaml` | Executor context demo |
| `examples_cpp_context_logger_cfg.yaml` | Logger context demo |
| `examples_cpp_context_publisher_cfg.yaml` | Transport publisher |
| `examples_cpp_context_subscriber_inline_cfg.yaml` | Subscriber (inline) |
| `examples_cpp_context_subscriber_on_executor_cfg.yaml` | Subscriber (on executor) |
| `examples_cpp_context_rpc_client_cfg.yaml` | RPC client |
| `examples_cpp_context_rpc_server_inline_cfg.yaml` | RPC server (inline) |
| `examples_cpp_context_rpc_server_on_executor_cfg.yaml` | RPC server (on executor) |

## Concepts Demonstrated

- `ctx_ptr_->LetMe()` -- establish thread-local context ownership
- `ctx_ptr_->GetExecutor()` -- get executor from context
- `ctx_ptr_->GetLogger()` -- get logger from context
- `IBMW_HL_INFO(logger, ...)` -- context-aware logging
- Transport pub/sub through context handle
- RPC client/server through context handle
- Coroutine-based RPC loops with `co_await`
- Resource isolation between modules via context IDs

## Note on ibmw-gen

This demo uses the **Context API** (`ibmw::context::Context`), a higher-level
abstraction layer distinct from the Runtime API used by the other sample demos.
The `ibmw-gen` code generator targets the Runtime API and does not generate
Context API style code. To learn about ibmw-gen for Runtime API based projects,
see the [ibmw-gen README](../../../tools/ibmw_gen/README.md).
