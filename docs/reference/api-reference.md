# IBMW API Reference

Complete reference for the IBMW C++ SDK, C ABI, extension API, and package API.

## Table of Contents

1. [C++ SDK](#c-sdk)
   - [Runtime](#runtime)
   - [Module](#module)
   - [Configurator](#configurator)
   - [Executor](#executor)
   - [Timer](#timer)
   - [Logger](#logger)
   - [Transport](#transport)
   - [Transport Context](#transport-context)
   - [RPC Service](#rpc-service)
   - [RPC Context](#rpc-context)
   - [RPC Status](#rpc-status)
   - [RPC Filter](#rpc-filter)
   - [Parameters](#parameters)
   - [Allocator](#allocator)
   - [Context API](#context-api)
   - [Coroutine Utilities](#coroutine-utilities)
   - [Buffer Utilities](#buffer-utilities)
   - [Function](#function)
   - [Type Support](#type-support)
   - [JSON Type Support](#json-type-support)
2. [Extension API](#extension-api)
3. [Package ABI](#package-abi)
4. [Type Support Package ABI](#type-support-package-abi)
5. [Driver-Slot ABI and Marker Dictionary](#driver-slot-abi-and-marker-dictionary)
6. [Architecture Diagram](#architecture-diagram)

---

## C++ SDK

All C++ SDK types are in the `ibmw` namespace unless otherwise noted.
Headers are under `src/api/cpp/`.

---

### Runtime

**Header:** `src/api/cpp/core.h`

The main handle to the IBMW framework. Modules receive this in `Setup()`.

```cpp
struct ModuleDescriptor {
  ibmw_string_view_t name;
  uint32_t major_version, minor_version, patch_version, build_version;
  ibmw_string_view_t author;
  ibmw_string_view_t description;
};
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Runtime()` | Default constructor | Empty handle |
| `Runtime(const ibmw_core_base_t*)` | Constructor | Wraps a C ABI core pointer |
| `operator bool` | `explicit operator bool() const` | True if handle is valid |
| `NativeHandle` | `const ibmw_core_base_t*` | Returns C ABI pointer |
| `GetInfo` | `ModuleDescriptor GetInfo() const` | Module descriptor |
| `GetConfigurator` | `ConfiguratorRef` | Configuration subsystem |
| `GetExecutor` | `ExecutorBusRef` | Executor bus subsystem |
| `GetLogger` | `LoggerRef` | Logger subsystem |
| `GetMemory` | `AllocatorRef` | Memory allocator |
| `GetService` | `ServiceRef` | RPC service subsystem |
| `GetTransport` | `TransportRef` | Transport subsystem |
| `GetParams` | `ParamsRef` | Parameter store |

---

### Module

**Header:** `src/api/cpp/module_base.h`

Abstract base class for user modules.

| Method | Signature | Description |
|--------|-----------|-------------|
| `Setup` | `virtual bool Setup(Runtime) = 0` | Register types, get handles |
| `Run` | `virtual bool Run() = 0` | Begin active work |
| `Stop` | `virtual void Stop() = 0` | Release resources |
| `Info` | `virtual ModuleDescriptor Info() const = 0` | Module metadata |
| `NativeHandle` | `const ibmw_module_base_t*` | C ABI pointer |

---

### Configurator

**Header:** `src/api/cpp/configurator/configurator.h`

Read-only handle to per-module configuration.

| Method | Signature | Description |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | True if valid |
| `GetConfigFilePath` | `std::string_view` | Path to module YAML config |

---

### Executor

**Header:** `src/api/cpp/executor/executor.h`, `executor_bus.h`

```cpp
using ExecutorTask = ibmw::util::Function<ibmw_function_executor_task_ops_t>;
```

#### ExecutorHandle

| Method | Signature | Description |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | True if valid |
| `Type` | `std::string_view` | Executor type ("asio", "tbb") |
| `Name` | `std::string_view` | Executor instance name |
| `IsThreadSafe` | `bool` | Whether this executor is thread-safe |
| `IsInCurrentExecutor` | `bool` | Whether current thread is in this executor |
| `SupportsTimedDispatch` | `bool` | Whether timed dispatch is supported |
| `Dispatch` | `void Dispatch(ExecutorTask&&)` | Submit a task |
| `Now` | `uint64_t` | Current nanosecond timestamp |
| `DispatchAtNs` | `void DispatchAtNs(uint64_t tp, ExecutorTask&&)` | Schedule at time point |

#### ExecutorBusRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetExecutor` | `ExecutorHandle GetExecutor(std::string_view name)` | Get executor by name |

---

### Timer

**Header:** `src/api/cpp/executor/timer.h`

#### TimerBase

| Method | Signature | Description |
|--------|-----------|-------------|
| `TimerBase(ExecutorHandle)` | Constructor | Bind to executor |
| `SetPeriodNs` | `void SetPeriodNs(uint64_t)` | Set period in nanoseconds |
| `GetPeriodNs` | `uint64_t` | Get period in nanoseconds |

#### Timer\<TaskType\>

| Method | Signature | Description |
|--------|-----------|-------------|
| `Start` | `void Start()` | Begin repeating timer |
| `SetTask` | `void SetTask(TaskType&&)` | Set callback |

#### Free Function

```cpp
Timer<ExecutorTask> CreateTimer(ExecutorHandle executor);
```

---

### Logger

**Header:** `src/api/cpp/logger/logger.h`

```cpp
enum ibmw_log_level_t {
  IBMW_LOG_LEVEL_TRACE = 0, IBMW_LOG_LEVEL_DEBUG = 1,
  IBMW_LOG_LEVEL_INFO = 2,  IBMW_LOG_LEVEL_WARN = 3,
  IBMW_LOG_LEVEL_ERROR = 4, IBMW_LOG_LEVEL_FATAL = 5,
  IBMW_LOG_LEVEL_OFF = 6,
};
```

#### LoggerRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | True if valid |
| `GetLogLevel` | `ibmw_log_level_t` | Current log level |
| `Log` | `void Log(level, line, file, func, data, size)` | Emit log entry |

#### Free Function

```cpp
LoggerRef& GetSimpleLoggerRef();  // Global simple logger
```

---

### Transport

**Header:** `src/api/cpp/transport/transport_handle.h`

#### SenderRef (Publisher)

| Method | Signature | Description |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | True if valid |
| `RegisterSendType` | `bool RegisterSendType(const ibmw_type_support_base_t*)` | Register message type |
| `Send` | `void Send(std::string_view type, ContextRef ctx, const void* msg)` | Publish a message |
| `GetTopic` | `std::string_view` | Topic name |

#### ReceiverRef (Subscriber)

| Method | Signature | Description |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | True if valid |
| `GetTopic` | `std::string_view` | Topic name |

#### TransportRef (Bus)

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetSender` | `SenderRef GetSender(std::string_view topic)` | Get publisher for topic |
| `GetReceiver` | `ReceiverRef GetReceiver(std::string_view topic)` | Get subscriber for topic |

#### Convenience Functions

```cpp
template <typename T>
bool RegisterSendType(SenderRef sender);

template <typename T>
void Send(SenderRef sender, const T& msg);

template <typename T>
void OnMessage(ReceiverRef receiver, std::function<void(const T&)> callback);
```

---

### Transport Context

**Header:** `src/api/cpp/transport/transport_context.h`

Carries metadata as key-value pairs on published/received messages.

Built-in keys:
- `ibmw-serialization_type` -- Serialization format
- `ibmw-driver` -- Transport driver name
- `ibmw-to_addr` -- Target address
- `ibmw-pub_seq` -- Publisher sequence number
- `ibmw-pub_ts` -- Publisher timestamp

```cpp
enum ibmw_transport_context_type_t {
  IBMW_TRANSPORT_PUBLISHER_CONTEXT = 0,
  IBMW_TRANSPORT_SUBSCRIBER_CONTEXT = 1,
};
```

#### ContextRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetType` | `ibmw_transport_context_type_t` | Publisher or subscriber |
| `GetMetaVal` | `std::string_view GetMetaVal(std::string_view key)` | Get metadata value |
| `SetMetaVal` | `void SetMetaVal(std::string_view key, std::string_view val)` | Set metadata |
| `GetMetaKeys` | `ibmw_string_view_array_t` | All metadata keys |
| `ToString` | `std::string` | Human-readable string |

#### Context (owning)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Context(type)` | Constructor | Create owned context |
| `GetContextRef` | `ContextRef` | Get lightweight ref |

---

### RPC Service

**Header:** `src/api/cpp/service/rpc_handle.h`

```cpp
using ServiceCallback = ibmw::util::Function<...>;  // void()
using ClientCallback = ibmw::util::Function<...>;    // void(rpc::Status)
using ServiceFunc = ibmw::util::Function<...>;       // void(ContextRef, const void*, void*, ServiceCallback&&)
```

#### ServiceRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `RegisterServiceFunc` | `bool RegisterServiceFunc(func_name, custom_ts, req_ts, rsp_ts, handler)` | Register server |
| `RegisterClientFunc` | `bool RegisterClientFunc(func_name, custom_ts, req_ts, rsp_ts)` | Register client |
| `Call` | `void Call(func_name, ctx, req, rsp, callback)` | Invoke RPC |

#### Access Patterns

| Class | Method | Description |
|-------|--------|-------------|
| `SyncAccessBase<Req, Rsp>` | `rpc::Status Invoke(ctx, req, rsp)` | Blocking call |
| `AsyncAccessBase<Req, Rsp>` | `void Invoke(ctx, req, rsp, callback)` | Callback-based |
| `FutureAccessBase<Req, Rsp>` | `std::future<Status> Invoke(ctx, req, rsp)` | Future-based |
| `CoAccessBase<Req, Rsp>` | `co::Task<Status> Invoke(ctx, req, rsp)` | Coroutine-based |

---

### RPC Context

**Header:** `src/api/cpp/service/rpc_context.h`

Similar to transport context but for RPC calls.

Built-in keys:
- `ibmw-to_addr` -- Target address
- `ibmw-serialization_type` -- Serialization format
- `ibmw-function_name` -- RPC function name
- `ibmw-driver` -- RPC driver name
- `ibmw-request_id` -- Request ID
- `ibmw-response_id` -- Response ID

```cpp
enum ibmw_rpc_context_type_t {
  IBMW_RPC_CLIENT_CONTEXT = 0,
  IBMW_RPC_SERVER_CONTEXT = 1,
};
```

#### ContextRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetType` | `ibmw_rpc_context_type_t` | Client or server |
| `GetTimeoutNs` | `uint64_t` | Timeout in nanoseconds |
| `SetTimeoutNs` | `void SetTimeoutNs(uint64_t)` | Set timeout |
| `GetMetaVal` | `std::string_view GetMetaVal(key)` | Get metadata |
| `SetMetaVal` | `void SetMetaVal(key, val)` | Set metadata |
| `ToString` | `std::string` | Human-readable string |

---

### RPC Status

**Header:** `src/api/cpp/service/rpc_status.h`

| Method | Signature | Description |
|--------|-----------|-------------|
| `Status()` | Constructor | OK status (code 0) |
| `Status(uint32_t code)` | Constructor | Specific status code |
| `OK` | `bool OK() const` | True if code is 0 |
| `Code` | `uint32_t Code() const` | Raw status code |

Status codes:

| Code | Name | Description |
|------|------|-------------|
| 0 | `OK` | Success |
| 1 | `UNKNOWN` | Unknown error |
| 2 | `TIMEOUT` | Request timed out |
| 1000-1009 | `SVR_*` | Server-side errors |
| 2000-2011 | `CLI_*` | Client-side errors |

---

### RPC Filter

**Header:** `src/api/cpp/service/rpc_co_filter.h`

Coroutine-based RPC middleware chain.

#### CoRpcHandle

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetServiceContext` | `rpc::ContextRef` | RPC context |
| `GetReqPtr` | `const void*` | Request message pointer |
| `GetRspPtr` | `void*` | Response message pointer |
| `GetStatus` | `rpc::Status` | Current status |
| `SetStatus` | `void SetStatus(rpc::Status)` | Override status |

#### CoRpcFilter (abstract)

```cpp
virtual co::Task<rpc::Status> Handle(CoRpcHandle& handle, CoFilterBus& next) = 0;
```

#### CoFilterBus

| Method | Signature | Description |
|--------|-----------|-------------|
| `Next` | `co::Task<rpc::Status> Next(CoRpcHandle&)` | Advance to next filter |

---

### Parameters

**Header:** `src/api/cpp/parameter/parameter_handle.h`

#### ParamsRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetParameter` | `std::string GetParameter(std::string_view key)` | Get value |
| `SetParameter` | `void SetParameter(std::string_view key, std::string_view val)` | Set value |

---

### Allocator

**Header:** `src/api/cpp/memory/allocator.h`

#### AllocatorRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetThreadLocalBuf` | `void* GetThreadLocalBuf(size_t)` | Thread-local buffer |
| `GetThreadLocalBuf<T>` | `T* GetThreadLocalBuf<T>(size_t)` | Typed thread-local buffer |

---

### Context API

**Header:** `src/api/cpp/context/context.h`

High-level builder-pattern API for resource-managed module development.

#### Context

| Method | Signature | Description |
|--------|-----------|-------------|
| `Context()` | Constructor | Empty context |
| `Context(Runtime, executor_name)` | Constructor | Bind to runtime and executor |
| `Pub<T>(topic)` | `OpPub<T>` | Publisher builder |
| `Sub<T>(topic)` | `OpSub<T>` | Subscriber builder |
| `Cli<Req, Rsp>(func)` | `OpCli<Req, Rsp>` | Client builder |
| `Srv<Req, Rsp>(func)` | `OpSrv<Req, Rsp>` | Server builder |
| `Log()` | `OpLog` | Logger builder |
| `GetRuntime` | `Runtime` | Underlying runtime |
| `GetExecutor` | `ExecutorHandle` | Bound executor |
| `SetExecutor` | `void SetExecutor(name)` | Change executor |
| `Shutdown` | `void Shutdown()` | Stop all resources |

#### OpPub\<T\>

| Method | Signature | Description |
|--------|-----------|-------------|
| `Use(pub)` | `OpPub&` | Bind output resource handle |
| `Build` | `bool` | Register and finalize |

#### OpSub\<T\>

| Method | Signature | Description |
|--------|-----------|-------------|
| `Use(sub)` | `OpSub&` | Bind output resource handle |
| `SetCallback(f)` | `OpSub&` | Set message callback |
| `Build` | `bool` | Register and finalize |

Supported callback shapes:
- `void(const T&)`
- `void(const std::shared_ptr<const T>&)`
- `void(transport::ContextRef, const T&)`
- `void(transport::ContextRef, const std::shared_ptr<const T>&)`

#### OpCli\<Req, Rsp\>

| Method | Signature | Description |
|--------|-----------|-------------|
| `Use(cli)` | `OpCli&` | Bind output resource handle |
| `Build` | `bool` | Register and finalize |

#### OpSrv\<Req, Rsp\>

| Method | Signature | Description |
|--------|-----------|-------------|
| `Use(srv)` | `OpSrv&` | Bind output resource handle |
| `SetCallback(f)` | `OpSrv&` | Set server handler |
| `AddFilter(filter)` | `OpSrv&` | Add coroutine filter |
| `Build` | `bool` | Register and finalize |

Supported server callback shapes:
- `rpc::Status(const Req&, Rsp&)`
- `rpc::Status(rpc::ContextRef, const Req&, Rsp&)`
- `void(const Req&, Rsp&, ServiceCallback&&)`
- `co::Task<rpc::Status>(const Req&, Rsp&)`
- `co::Task<rpc::Status>(rpc::ContextRef, const Req&, Rsp&)`

#### Resource Handles

| Class | Method | Description |
|-------|--------|-------------|
| `res::Publisher<T>` | `Publish(msg)` | Publish with default context |
| `res::Publisher<T>` | `Publish(ctx, msg)` | Publish with explicit context |
| `res::Client<Req, Rsp>` | `Call(ctx, req, rsp)` | Sync RPC call |
| `res::Client<Req, Rsp>` | `CallAsync(ctx, req, rsp, cb)` | Async RPC call |
| `res::Client<Req, Rsp>` | `CallFuture(ctx, req, rsp)` | Future RPC call |
| `res::Client<Req, Rsp>` | `CallCo(ctx, req, rsp)` | Coroutine RPC call |

All resource handles have `IsEnabled()` from the base class.

---

### Coroutine Utilities

**Headers:** `src/api/cpp/co/`

The `ibmw::co` namespace wraps either libunifex or stdexec based on build config.

| Symbol | Description |
|--------|-------------|
| `Task<T>` | Lazy coroutine task returning T |
| `IbmwExecutor` | Wraps `ExecutorHandle` as a coroutine scheduler |
| `IbmwContext` | Coroutine execution context |
| `AsyncScope` | Structured concurrency scope |
| `Schedule(executor)` | Schedule on executor |
| `ScheduleAfter(executor, duration)` | Schedule after delay |
| `ScheduleAt(executor, time_point)` | Schedule at time |
| `SyncWait(sender)` | Block until sender completes |
| `StartDetached(sender)` | Fire-and-forget |
| `Then(sender, func)` | Chain continuation |
| `On(scheduler, sender)` | Transfer execution |
| `InlineExecutor` | Runs tasks inline |

---

### Buffer Utilities

**Header:** `src/api/cpp/util/buffer.h`, `buffer_array_allocator.h`

#### BufferArray (owning)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Size` | `size_t` | Number of buffers |
| `Capacity` | `size_t` | Capacity |
| `Reserve` | `void Reserve(size_t)` | Pre-allocate |
| `NewBuffer` | `ibmw_buffer_t NewBuffer(size_t)` | Allocate buffer |
| `BufferSize` | `size_t` | Total byte count |
| `JoinToString` | `std::string` | Concatenate all buffers |

#### BufferArrayView (non-owning)

| Method | Signature | Description |
|--------|-----------|-------------|
| `Size` | `size_t` | Number of views |
| `BufferSize` | `size_t` | Total byte count |
| `JoinToString` | `std::string` | Concatenate all views |
| `JoinToCharVector` | `std::vector<char>` | Concatenate to vector |

#### Allocators

| Class | Description |
|-------|-------------|
| `SimpleBufferArrayAllocator` | Heap-based (malloc/free) |
| `FlatBufferArrayAllocator` | Arena-based from pre-allocated memory |

---

### Function

**Header:** `src/api/cpp/util/function.h`

ABI-safe type-erased callable with small-buffer optimization (3 pointers inline).

```cpp
template <typename Ops>
class Function;
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `Function()` | Constructor | Empty (null) |
| `Function(callable)` | Constructor | Type-erase a callable |
| `operator()` | `R operator()(Args...)` | Invoke |
| `operator bool` | `explicit operator bool()` | True if non-null |
| `NativeHandle` | `ibmw_function_base_t*` | C ABI pointer |

---

### Type Support

**Header:** `src/api/cpp/util/type_support.h`

#### TypeSupportRef

| Method | Signature | Description |
|--------|-----------|-------------|
| `TypeName` | `std::string_view` | Type name |
| `Create` | `void*` | Allocate message instance |
| `Destroy` | `void Destroy(void*)` | Deallocate message |
| `CreateSharedPtr` | `std::shared_ptr<void>` | Shared-ptr message |
| `Copy` | `void Copy(from, to)` | Copy message |
| `Move` | `void Move(from, to)` | Move message |
| `Serialize` | `bool Serialize(type, msg, alloc, buf)` | Serialize message |
| `Deserialize` | `bool Deserialize(type, buf, msg)` | Deserialize message |
| `DefaultSerializationType` | `std::string_view` | Default format |
| `CheckSerializationTypeSupported` | `bool CheckSerializationTypeSupported(type)` | Check format |
| `SimpleSerialize` | `std::string SimpleSerialize(type, msg)` | Serialize to string |

---

### JSON Type Support

**Header:** `src/api/cpp/util/json_type_support.h`

```cpp
template <typename JsonType>
const ibmw_type_support_base_t* GetJsonMessageTypeSupport();
```

Generates type support for any struct with `static constexpr char kTypeName[]`
and nlohmann `to_json`/`from_json` ADL functions. Serialization type: `"json"`.

---

## Extension API

**Headers:** `src/core/extension_api/`

### EngineExtensionBase

| Method | Signature | Description |
|--------|-----------|-------------|
| `Name` | `virtual std::string_view Name() const noexcept = 0` | Extension name |
| `Initialize` | `virtual bool Initialize(Engine*) noexcept = 0` | Init with engine |
| `Shutdown` | `virtual void Shutdown() noexcept = 0` | Clean up |
| `GenInitializationReport` | `virtual list<pair<string,string>>` | Startup log data |

### Entry Point

```cpp
// Must be exported from extension .so
IBMW_CORE_EXTENSION_EXPORT
EngineExtensionBase* IbmwDynlibCreateCoreExtensionHandle();

IBMW_CORE_EXTENSION_EXPORT
void IbmwDynlibDestroyCoreExtensionHandle(const EngineExtensionBase*);
```

### Macros

| Macro | Description |
|-------|-------------|
| `IBMW_CORE_EXTENSION_EXPORT` | Export visibility attribute |
| `IBMW_DECLARE_EXTENSION_LOGGER(ns)` | Declare per-extension logger |
| `IBMW_DEFINE_EXTENSION_LOGGER(ns)` | Define per-extension logger |

---

## Package ABI

**Headers:** `src/api/pkg/`

Module packages are shared libraries containing one or more IBMW modules.

### Entry Points

```cpp
typedef struct {
    size_t (*get_module_num)();
    const ibmw_string_view_t* (*get_module_name_list)();
    const ibmw_module_base_t* (*create_module)(ibmw_string_view_t module_name);
    void (*destroy_module)(const ibmw_module_base_t* module_ptr);
    void (*set_driver_slot)(ibmw_string_view_t module_name,
                            ibmw_driver_slot_t slot_id,
                            void* driver_ptr);
} ibmw_pkg_entry_t;

IBMW_PKG_EXPORT ibmw_pkg_entry_t IbmwPkgEntry();
```

### Convenience Macro

```cpp
static const auto kModuleArray = std::array{
    std::make_tuple("MyModule", []() -> ibmw::Module* { return new MyModule(); }),
};
IBMW_PKG_MAIN(kModuleArray)
```

Generates the single `IbmwPkgEntry` symbol automatically.

---

## Type Support Package ABI

**Headers:** `src/api/type_support/`

Type support packages provide serialization for message types at runtime.

### Entry Points

```cpp
IBMW_TYPE_SUPPORT_PKG_EXPORT
const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1();
```

The descriptor contains the TypeSupport array and, when available, an
embedded `ibmw_schema_support_package_v1_t` containing immutable
`ibmw_schema_support_v1_t` descriptors.  Both structures begin with
`abi_version` and `struct_size`; consumers must verify the version and ensure
`struct_size` is at least the corresponding `*_V1_REQUIRED_SIZE` before reading
the mandatory fields.  Producers may append fields in a future ABI revision
without changing the existing prefix.  String views and schema bytes remain
owned by the package until it is unloaded.  V1 uses canonical lower-case
serialization/schema encodings and an `fnv1a64:<16 lowercase hex digits>` hash
over the exact schema bytes; the hash is an accelerator, not a collision-free
identity.  Empty required strings or schema data are invalid.

---

## Driver-Slot ABI and Marker Dictionary

The driver-slot ABI and the marker public dictionary form the central
API surface that decouples user-region modules from transport drivers.
This section lists the headers, macros, and symbols you can quote
directly.

### `src/api/c/driver_slot.h` -- Slot ID Entry Points

```c
typedef uint64_t ibmw_driver_slot_t;

// Stable hash from a driver name to a slot id (computed at compile time).
#define IBMW_DRIVER_ID(name)            /* constexpr ibmw_driver_slot_t */
#define IBMW_DECLARE_DRIVER_SLOT(name)  /* injects kStableDriverId into a Driver class */
```

Each of the nine `*TransportDriver` subclasses (local / tcp / http / udp /
dds / mqtt / ros2 / iceoryx / zenoh) declares a stable slot id via
`IBMW_DECLARE_DRIVER_SLOT(...)`. That id flows through the broadcast chain
the Engine fires at `Start()`:
`engine.cc::Start()` -> `module_bus.cc::BroadcastDriverSlot` ->
`package_loader.cc::PackageHandle::SetDriverSlot`.

### `src/api/pkg/pkg_main.h` -- Package ABI

```c
typedef struct {
    /* ... module entry points ... */
    void (*set_driver_slot)(const char* module_name,
                            ibmw_driver_slot_t slot_id,
                            void* driver_ptr);
} ibmw_pkg_entry_t;
```

`IBMW_PKG_MAIN(...)` in `src/api/pkg/pkg_main.h` exports the
entry symbol `IbmwPkgEntry` (typed slot). It must be visible via `nm -D`
from every pkg `.so`.

### `src/api/cpp/module_base.h` -- `IBMW_USE_DRIVER` / `IBMW_USE_DRIVERS`

```cpp
// Single-driver injection.
#define IBMW_USE_DRIVER(DriverT, member_ptr)

// Multi-driver injection (variadic).
#define IBMW_USE_DRIVERS(...)                  // pass any number of IBMW_DRIVER_ENTRY
#define IBMW_DRIVER_ENTRY(DriverT, member_ptr) // a single entry; expands to a bool-valued lambda
```

Both macros emit an override of `Module::SetDriverSlot(slot_id, driver_ptr)`.
`IBMW_USE_DRIVERS` expands its arguments through `bool results[] = { __VA_ARGS__ };`
to dodge the comma-operator pitfall (each entry is already a `bool` value,
not a callable).

Constraints:
- Each `DriverT` must publish `static constexpr const char* kStableDriverId`
  (auto-injected by `IBMW_DECLARE_DRIVER_SLOT` in the driver header).
- `member_ptr` must be assignable from `DriverT*` (typically a `T*` member).
- At least one `IBMW_DRIVER_ENTRY` is required; zero entries are illegal
  (use the base-class no-op default for that case).

### `src/api/cpp/marker.h` -- Marker Grammar Public Dictionary

```cpp
namespace ibmw::api::cpp::marker {

// 5 non-RPC marker classes (user-region / abi / ...) + 1 RPC class.
inline constexpr const char kUserRegionBegin[]   = "...";
inline constexpr const char kUserRegionEnd[]     = "...";
// ... full set of six token sequences, see header for details.
}  // namespace ibmw::api::cpp::marker
```

An inline-constexpr dictionary that is the single source of truth for
the byte sequences shared between ibmw-gen's emit sites and any
hand-written code that mimics generator output.

> Any user-region code that needs to reference a marker (typically a
> hand-crafted sample mimicking ibmw-gen output) **must**
> `#include "cpp/marker.h"` and reuse the `constexpr` strings.
> Hard-coding a token in a string literal will break ibmw-gen
> byte-equality with the in-tree regenerated sources.

### Driver-Slot Call Sequence

```
Engine::Start()
    |
    v
ModuleBus::BroadcastDriverSlot(slot_id, driver_ptr)
    |  for each PackageHandle:
    v
PackageHandle::SetDriverSlot(module_name, slot_id, driver_ptr)
    |  -> IbmwPkgEntry.set_driver_slot
    v
Module::SetDriverSlot(slot_id, driver_ptr)        // override emitted by IBMW_USE_DRIVER(S)
    |  fold over the IBMW_DRIVER_ENTRY list
    |  if (slot_id == DriverT::kStableDriverId) {
    |      member = static_cast<DriverT*>(driver_ptr);
    |  }
```

---

## Architecture Diagram

```
+-------------------------------------------------------------------+
|  User Module Code                                                  |
|  (subclasses ibmw::Module, uses ibmw::context::Context)           |
+-------------------------------------------------------------------+
|  C++ SDK Layer (ibmw::*)                                           |
|  Runtime, Context, OpPub/Sub/Cli/Srv, res::Publisher/Client...     |
|  Coroutine: co::Task<T>, Schedule, SyncWait, AsyncScope            |
|  Util: Function<Ops>, BufferArray, TypeSupportRef                  |
+-------------------------------------------------------------------+
|  C ABI Layer (ibmw_*_base_t vtables)                               |
|  ibmw_core_base_t, ibmw_module_base_t, ibmw_executor_base_t,      |
|  ibmw_transport_*_base_t, ibmw_service_*_base_t, etc.              |
+-------------------------------------------------------------------+
|  Engine Core + Extensions                                          |
|  EngineExtensionBase, transport/RPC drivers as dynlibs             |
+-------------------------------------------------------------------+
|  Pkg ABI / Type Support Pkg ABI                                    |
|  IbmwDynlib* entry points for module & type support packages       |
+-------------------------------------------------------------------+
```

**Key design patterns:**

- **C ABI bridge:** Every C++ `*Ref` class wraps a `*_base_t` vtable struct,
  enabling ABI stability across shared library boundaries.
- **Builder pattern:** `OpPub`/`OpSub`/`OpCli`/`OpSrv` provide fluent API for
  resource registration.
- **Four RPC modes:** Sync, Async, Future, Coroutine access patterns.
- **Coroutine abstraction:** `ibmw::co` wraps either libunifex or stdexec.
- **SBO Function:** `ibmw::util::Function<Ops>` uses small-buffer optimization
  with a C-compatible ops vtable for cross-library type erasure.
- **PMR contexts:** Transport and RPC contexts use polymorphic memory resources
  for small-buffer-optimized metadata storage.
