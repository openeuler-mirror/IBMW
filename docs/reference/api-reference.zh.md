# IBMW API 参考

IBMW C++ SDK、C ABI、Extension API 与 Package API 的完整参考。

## 目录

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
5. [Driver-Slot ABI 与 Marker 字典](#driver-slot-abi-与-marker-字典)
6. [架构图](#架构图)

---

## C++ SDK

除非另有说明，所有 C++ SDK 类型都位于 `ibmw` 命名空间。
头文件位于 `src/api/cpp/`。

---

### Runtime

**头文件**：`src/api/cpp/core.h`

IBMW 框架的主句柄。Module 在 `Setup()` 中接收它。

```cpp
struct ModuleDescriptor {
  ibmw_string_view_t name;
  uint32_t major_version, minor_version, patch_version, build_version;
  ibmw_string_view_t author;
  ibmw_string_view_t description;
};
```

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Runtime()` | 默认构造 | 空句柄 |
| `Runtime(const ibmw_core_base_t*)` | 构造 | 包装一个 C ABI core 指针 |
| `operator bool` | `explicit operator bool() const` | 句柄是否有效 |
| `NativeHandle` | `const ibmw_core_base_t*` | 返回 C ABI 指针 |
| `GetInfo` | `ModuleDescriptor GetInfo() const` | 模块描述符 |
| `GetConfigurator` | `ConfiguratorRef` | 配置子系统 |
| `GetExecutor` | `ExecutorBusRef` | Executor 总线子系统 |
| `GetLogger` | `LoggerRef` | 日志子系统 |
| `GetMemory` | `AllocatorRef` | 内存分配器 |
| `GetService` | `ServiceRef` | RPC 服务子系统 |
| `GetTransport` | `TransportRef` | Transport 子系统 |
| `GetParams` | `ParamsRef` | 参数存储 |

---

### Module

**头文件**：`src/api/cpp/module_base.h`

用户模块的抽象基类。

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Setup` | `virtual bool Setup(Runtime) = 0` | 注册类型、获取句柄 |
| `Run` | `virtual bool Run() = 0` | 开始活跃工作 |
| `Stop` | `virtual void Stop() = 0` | 释放资源 |
| `Info` | `virtual ModuleDescriptor Info() const = 0` | 模块元数据 |
| `NativeHandle` | `const ibmw_module_base_t*` | C ABI 指针 |

---

### Configurator

**头文件**：`src/api/cpp/configurator/configurator.h`

per-module 配置的只读句柄。

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | 是否有效 |
| `GetConfigFilePath` | `std::string_view` | 模块 YAML 配置路径 |

---

### Executor

**头文件**：`src/api/cpp/executor/executor.h`、`executor_bus.h`

```cpp
using ExecutorTask = ibmw::util::Function<ibmw_function_executor_task_ops_t>;
```

#### ExecutorHandle

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | 是否有效 |
| `Type` | `std::string_view` | Executor 类型（"asio"、"tbb"）|
| `Name` | `std::string_view` | Executor 实例名 |
| `IsThreadSafe` | `bool` | 该 executor 是否线程安全 |
| `IsInCurrentExecutor` | `bool` | 当前线程是否在该 executor 中 |
| `SupportsTimedDispatch` | `bool` | 是否支持定时 dispatch |
| `Dispatch` | `void Dispatch(ExecutorTask&&)` | 提交任务 |
| `Now` | `uint64_t` | 当前纳秒时间戳 |
| `DispatchAtNs` | `void DispatchAtNs(uint64_t tp, ExecutorTask&&)` | 定时 dispatch |

#### ExecutorBusRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetExecutor` | `ExecutorHandle GetExecutor(std::string_view name)` | 按名取 executor |

---

### Timer

**头文件**：`src/api/cpp/executor/timer.h`

#### TimerBase

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `TimerBase(ExecutorHandle)` | 构造 | 绑定到 executor |
| `SetPeriodNs` | `void SetPeriodNs(uint64_t)` | 设置纳秒周期 |
| `GetPeriodNs` | `uint64_t` | 获取纳秒周期 |

#### Timer\<TaskType\>

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Start` | `void Start()` | 开始重复定时器 |
| `SetTask` | `void SetTask(TaskType&&)` | 设置回调 |

#### 自由函数

```cpp
template <typename TaskType>
[[nodiscard]] std::shared_ptr<TimerBase> CreateTimer(ExecutorHandle executor,
                                                     std::chrono::nanoseconds period,
                                                     TaskType&& task, bool auto_start = true);
```

返回的 `shared_ptr` 持有定时器；最后一个副本释放后定时器即停止。

---

### Logger

**头文件**：`src/api/cpp/logger/logger.h`

```cpp
enum ibmw_log_level_t {
  IBMW_LOG_LEVEL_TRACE = 0, IBMW_LOG_LEVEL_DEBUG = 1,
  IBMW_LOG_LEVEL_INFO = 2,  IBMW_LOG_LEVEL_WARN = 3,
  IBMW_LOG_LEVEL_ERROR = 4, IBMW_LOG_LEVEL_FATAL = 5,
  IBMW_LOG_LEVEL_OFF = 6,
};
```

#### LoggerRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | 是否有效 |
| `GetLogLevel` | `ibmw_log_level_t` | 当前日志级别 |
| `Log` | `void Log(level, line, file, func, data, size)` | 输出日志 |

#### 自由函数

```cpp
LoggerRef& GetSimpleLoggerRef();  // 全局简易 logger
```

---

### Transport

**头文件**：`src/api/cpp/transport/transport_handle.h`

#### SenderRef（Publisher）

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | 是否有效 |
| `RegisterSendType` | `bool RegisterSendType(const ibmw_type_support_base_t*)` | 注册消息类型 |
| `Send` | `void Send(std::string_view type, ContextRef ctx, const void* msg)` | 发布消息 |
| `GetTopic` | `std::string_view` | Topic 名 |

#### ReceiverRef（Subscriber）

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `operator bool` | `explicit operator bool() const` | 是否有效 |
| `GetTopic` | `std::string_view` | Topic 名 |

#### TransportRef（Bus）

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetPublisher` | `SenderRef GetPublisher(std::string_view topic)` | 获取该 topic 的 publisher |
| `GetSubscriber` | `ReceiverRef GetSubscriber(std::string_view topic)` | 获取该 topic 的 subscriber |

#### 便捷函数

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

**头文件**：`src/api/cpp/transport/transport_context.h`

在发布/接收消息上以 key-value 形式携带元数据。

内置 key：
- `ibmw-serialization_type` —— 序列化格式
- `ibmw-driver` —— Transport driver 名
- `ibmw-to_addr` —— 目标地址
- `ibmw-pub_seq` —— Publisher 序列号
- `ibmw-pub_ts` —— Publisher 时间戳

```cpp
enum ibmw_transport_context_type_t {
  IBMW_TRANSPORT_PUBLISHER_CONTEXT = 0,
  IBMW_TRANSPORT_SUBSCRIBER_CONTEXT = 1,
};
```

#### ContextRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetType` | `ibmw_transport_context_type_t` | Publisher 还是 subscriber |
| `GetMetaVal` | `std::string_view GetMetaVal(std::string_view key)` | 取元数据值 |
| `SetMetaVal` | `void SetMetaVal(std::string_view key, std::string_view val)` | 设置元数据 |
| `GetMetaKeys` | `ibmw_string_view_array_t` | 全部元数据 key |
| `ToString` | `std::string` | 人类可读字符串 |

#### Context（owning）

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Context(type)` | 构造 | 创建拥有所有权的 context |
| `GetContextRef` | `ContextRef` | 获取轻量 ref |

---

### RPC Service

**头文件**：`src/api/cpp/service/rpc_handle.h`

```cpp
using ServiceCallback = ibmw::util::Function<...>;  // void()
using ClientCallback = ibmw::util::Function<...>;    // void(rpc::Status)
using ServiceFunc = ibmw::util::Function<...>;       // void(ContextRef, const void*, void*, ServiceCallback&&)
```

#### ServiceRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `RegisterServiceFunc` | `bool RegisterServiceFunc(func_name, custom_ts, req_ts, rsp_ts, handler)` | 注册 server |
| `RegisterClientFunc` | `bool RegisterClientFunc(func_name, custom_ts, req_ts, rsp_ts)` | 注册 client |
| `Call` | `void Call(func_name, ctx, req, rsp, callback)` | 调用 RPC |

#### 访问模式

| 类 | 方法 | 描述 |
|-------|--------|-------------|
| `SyncAccessBase<Req, Rsp>` | `rpc::Status Invoke(ctx, req, rsp)` | 阻塞调用 |
| `AsyncAccessBase<Req, Rsp>` | `void Invoke(ctx, req, rsp, callback)` | 回调式 |
| `FutureAccessBase<Req, Rsp>` | `std::future<Status> Invoke(ctx, req, rsp)` | future 式 |
| `CoAccessBase<Req, Rsp>` | `co::Task<Status> Invoke(ctx, req, rsp)` | 协程式 |

异步和 future 模式按地址把 `req`、`rsp` 交给 provider。使用引用版本时，调用方必须保证两者存活到回调执行或 future 就绪。生成的 `AsyncAccess`、`FutureAccess` 还接受 `std::shared_ptr<const Req>` 和 `std::shared_ptr<Rsp>`，这组重载会把两个对象保活到调用完成。`req` 或 `rsp` 为空时以 `IBMW_RPC_STATUS_CLI_UNKNOWN` 完成。

---

### RPC Context

**头文件**：`src/api/cpp/service/rpc_context.h`

类似 transport context，但用于 RPC 调用。

内置 key：
- `ibmw-to_addr` —— 目标地址
- `ibmw-serialization_type` —— 序列化格式
- `ibmw-function_name` —— RPC 函数名
- `ibmw-driver` —— RPC driver 名
- `ibmw-request_id` —— Request ID
- `ibmw-response_id` —— Response ID

```cpp
enum ibmw_rpc_context_type_t {
  IBMW_RPC_CLIENT_CONTEXT = 0,
  IBMW_RPC_SERVER_CONTEXT = 1,
};
```

#### ContextRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetType` | `ibmw_rpc_context_type_t` | Client 还是 server |
| `GetTimeoutNs` | `uint64_t` | 纳秒级超时 |
| `SetTimeoutNs` | `void SetTimeoutNs(uint64_t)` | 设置超时 |
| `GetMetaVal` | `std::string_view GetMetaVal(key)` | 取元数据 |
| `SetMetaVal` | `void SetMetaVal(key, val)` | 设置元数据 |
| `ToString` | `std::string` | 人类可读字符串 |

---

### RPC Status

**头文件**：`src/api/cpp/service/rpc_status.h`

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Status()` | 构造 | OK 状态（code 0）|
| `Status(uint32_t code)` | 构造 | 指定 status code |
| `OK` | `bool OK() const` | code 是否为 0 |
| `Code` | `uint32_t Code() const` | 原始 status code |

状态码：

| Code | 名称 | 描述 |
|------|------|-------------|
| 0 | `OK` | 成功 |
| 1 | `UNKNOWN` | 未知错误 |
| 2 | `TIMEOUT` | 请求超时 |
| 1000-1009 | `SVR_*` | 服务端错误 |
| 2000-2011 | `CLI_*` | 客户端错误 |

---

### RPC Filter

**头文件**：`src/api/cpp/service/rpc_co_filter.h`

基于协程的 RPC 中间件链。

#### CoRpcHandle

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetServiceContext` | `rpc::ContextRef` | RPC context |
| `GetReqPtr` | `const void*` | request 消息指针 |
| `GetRspPtr` | `void*` | response 消息指针 |
| `GetStatus` | `rpc::Status` | 当前 status |
| `SetStatus` | `void SetStatus(rpc::Status)` | 覆盖 status |

#### CoRpcFilter（抽象）

```cpp
virtual co::Task<rpc::Status> Handle(CoRpcHandle& handle, CoFilterBus& next) = 0;
```

#### CoFilterBus

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Next` | `co::Task<rpc::Status> Next(CoRpcHandle&)` | 推进到下一个 filter |

---

### Parameters

**头文件**：`src/api/cpp/parameter/parameter_handle.h`

#### ParamsRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetParameter` | `std::string GetParameter(std::string_view key)` | 取值 |
| `SetParameter` | `void SetParameter(std::string_view key, std::string_view val)` | 写值 |

---

### Allocator

**头文件**：`src/api/cpp/memory/allocator.h`

#### AllocatorRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `GetThreadLocalBuf` | `void* GetThreadLocalBuf(size_t)` | 线程本地缓冲 |
| `GetThreadLocalBuf<T>` | `T* GetThreadLocalBuf<T>(size_t)` | 类型化线程本地缓冲 |

---

### Context API

**头文件**：`src/api/cpp/context/context.h`

面向资源管理式模块开发的高层 builder API。

#### Context

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Context()` | 构造 | 空 context |
| `Context(Runtime, executor_name)` | 构造 | 绑定 runtime 与 executor |
| `Pub<T>(topic)` | `OpPub<T>` | Publisher builder |
| `Sub<T>(topic)` | `OpSub<T>` | Subscriber builder |
| `Cli<Req, Rsp>(func)` | `OpCli<Req, Rsp>` | Client builder |
| `Srv<Req, Rsp>(func)` | `OpSrv<Req, Rsp>` | Server builder |
| `Log()` | `OpLog` | Logger builder |
| `GetRuntime` | `Runtime` | 底层 runtime |
| `GetExecutor` | `ExecutorHandle` | 绑定的 executor |
| `SetExecutor` | `void SetExecutor(name)` | 切换 executor |
| `Shutdown` | `void Shutdown()` | 停止全部资源 |

#### OpPub\<T\>

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Use(pub)` | `OpPub&` | 绑定输出 resource handle |
| `Build` | `bool` | 注册并完成 |

#### OpSub\<T\>

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Use(sub)` | `OpSub&` | 绑定输出 resource handle |
| `SetCallback(f)` | `OpSub&` | 设置消息回调 |
| `Build` | `bool` | 注册并完成 |

支持的 callback 形态：
- `void(const T&)`
- `void(const std::shared_ptr<const T>&)`
- `void(transport::ContextRef, const T&)`
- `void(transport::ContextRef, const std::shared_ptr<const T>&)`

#### OpCli\<Req, Rsp\>

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Use(cli)` | `OpCli&` | 绑定输出 resource handle |
| `Build` | `bool` | 注册并完成 |

#### OpSrv\<Req, Rsp\>

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Use(srv)` | `OpSrv&` | 绑定输出 resource handle |
| `SetCallback(f)` | `OpSrv&` | 设置 server handler |
| `AddFilter(filter)` | `OpSrv&` | 添加协程 filter |
| `Build` | `bool` | 注册并完成 |

支持的 server callback 形态：
- `rpc::Status(const Req&, Rsp&)`
- `rpc::Status(rpc::ContextRef, const Req&, Rsp&)`
- `void(const Req&, Rsp&, ServiceCallback&&)`
- `co::Task<rpc::Status>(const Req&, Rsp&)`
- `co::Task<rpc::Status>(rpc::ContextRef, const Req&, Rsp&)`

#### Resource Handles

| 类 | 方法 | 描述 |
|-------|--------|-------------|
| `res::Publisher<T>` | `Publish(msg)` | 用默认 context 发布 |
| `res::Publisher<T>` | `Publish(ctx, msg)` | 用显式 context 发布 |
| `res::Client<Req, Rsp>` | `Call(ctx, req, rsp)` | 同步 RPC 调用 |
| `res::Client<Req, Rsp>` | `CallAsync(ctx, req, rsp, cb)` | 异步 RPC 调用 |
| `res::Client<Req, Rsp>` | `CallFuture(ctx, req, rsp)` | future RPC 调用 |
| `res::Client<Req, Rsp>` | `CallCo(ctx, req, rsp)` | 协程 RPC 调用 |

所有 resource handle 都从基类继承 `IsEnabled()`。

---

### Coroutine Utilities

**头文件**：`src/api/cpp/co/`

`ibmw::co` 命名空间根据构建配置封装 libunifex 或 stdexec。

| 符号 | 描述 |
|--------|-------------|
| `Task<T>` | 返回 T 的惰性协程任务 |
| `IbmwExecutor` | 把 `ExecutorHandle` 包装成协程 scheduler |
| `IbmwContext` | 协程执行上下文 |
| `AsyncScope` | 结构化并发 scope |
| `Schedule(executor)` | 调度到 executor |
| `ScheduleAfter(executor, duration)` | 延后调度 |
| `ScheduleAt(executor, time_point)` | 定点调度 |
| `SyncWait(sender)` | 阻塞直到 sender 完成 |
| `StartDetached(sender)` | fire-and-forget |
| `Then(sender, func)` | 链接续延 |
| `On(scheduler, sender)` | 转移执行 |
| `InlineExecutor` | 任务原地运行 |

---

### Buffer Utilities

**头文件**：`src/api/cpp/util/buffer.h`、`buffer_array_allocator.h`

#### BufferArray（owning）

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Size` | `size_t` | buffer 数 |
| `Capacity` | `size_t` | 容量 |
| `Reserve` | `void Reserve(size_t)` | 预分配 |
| `NewBuffer` | `ibmw_buffer_t NewBuffer(size_t)` | 分配 buffer |
| `BufferSize` | `size_t` | 总字节数 |
| `JoinToString` | `std::string` | 连接所有 buffer |

#### BufferArrayView（non-owning）

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Size` | `size_t` | view 数 |
| `BufferSize` | `size_t` | 总字节数 |
| `JoinToString` | `std::string` | 连接所有 view |
| `JoinToCharVector` | `std::vector<char>` | 连接到 vector |

#### Allocator

| 类 | 描述 |
|-------|-------------|
| `SimpleBufferArrayAllocator` | 堆式（malloc/free）|
| `FlatBufferArrayAllocator` | 基于预分配内存的 arena |

---

### Function

**头文件**：`src/api/cpp/util/function.h`

ABI 安全的类型擦除可调用对象，带 small-buffer 优化（内联 3 指针）。

```cpp
template <typename Ops>
class Function;
```

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Function()` | 构造 | 空（null）|
| `Function(callable)` | 构造 | 类型擦除一个 callable |
| `operator()` | `R operator()(Args...)` | 调用 |
| `operator bool` | `explicit operator bool()` | 是否非空 |
| `NativeHandle` | `ibmw_function_base_t*` | C ABI 指针 |

---

### Type Support

**头文件**：`src/api/cpp/util/type_support.h`

#### TypeSupportRef

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `TypeName` | `std::string_view` | 类型名 |
| `Create` | `void*` | 分配 message 实例 |
| `Destroy` | `void Destroy(void*)` | 释放 message |
| `CreateSharedPtr` | `std::shared_ptr<void>` | shared-ptr message |
| `Copy` | `void Copy(from, to)` | 拷贝 message |
| `Move` | `void Move(from, to)` | move message |
| `Serialize` | `bool Serialize(type, msg, alloc, buf)` | 序列化 message |
| `Deserialize` | `bool Deserialize(type, buf, msg)` | 反序列化 message |
| `DefaultSerializationType` | `std::string_view` | 默认格式 |
| `CheckSerializationTypeSupported` | `bool CheckSerializationTypeSupported(type)` | 检查格式 |
| `SimpleSerialize` | `std::string SimpleSerialize(type, msg)` | 序列化为 string |

---

### JSON Type Support

**头文件**：`src/api/cpp/util/json_type_support.h`

```cpp
template <typename JsonType>
const ibmw_type_support_base_t* GetJsonMessageTypeSupport();
```

为任何带 `static constexpr char kTypeName[]` 和 nlohmann
`to_json`/`from_json` ADL 函数的 struct 生成 type support。
序列化类型：`"json"`。

---

## Extension API

**头文件**：`src/core/extension_api/`

### ExtensionBase

| 方法 | 签名 | 描述 |
|--------|-----------|-------------|
| `Name` | `std::string_view Name() const noexcept` | Extension 名 |
| `OnInit` | `bool OnInit(runtime::core::Engine* core_ptr)` | 使用 engine 初始化 Extension |
| `OnShutdown` | `void OnShutdown()` | 清理 Extension 资源 |
| `GenInitializationReport` | `std::list<std::pair<std::string, std::string>> GenInitializationReport() const` | 启动日志数据 |

### 入口点

```cpp
// 在 extension .so 的全局作用域中使用
IBMW_EXT_ENTRY(MyExtension)
```

`ExtensionBase` 提供框架生命周期方法 `Initialize` 和 `Shutdown`；
Extension 实现 `OnInit` 和 `OnShutdown`。

### 宏

| 宏 | 描述 |
|-------|-------------|
| `IBMW_EXT_ENTRY(ExtensionClass)` | 定义带版本的 Extension 入口点 `IbmwExtEntryV3`（`IBMW_EXT_ENTRY_V2` 为别名）。基于旧版 IBMW 编译的 Extension 会被拒绝加载，需要重新编译 |
| `IBMW_CORE_EXTENSION_EXPORT` | 导出 visibility 属性 |
| `IBMW_DECLARE_EXTENSION_LOGGER(ns)` | 声明 per-extension logger |
| `IBMW_DEFINE_EXTENSION_LOGGER(ns)` | 定义 per-extension logger |

---

## Package ABI

**头文件**：`src/api/pkg/`

Module package 是包含一个或多个 IBMW module 的共享库。

### 入口点

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

### 便捷宏

```cpp
static const auto kModuleArray = std::array{
    std::make_tuple("MyModule", []() -> ibmw::Module* { return new MyModule(); }),
};
IBMW_PKG_MAIN(kModuleArray)
```

自动生成单一 `IbmwPkgEntry` 符号。

---

## Type Support Package ABI

**头文件**：`src/api/type_support/`

Type support package 在运行时为消息类型提供序列化能力。

### 入口点

```cpp
IBMW_TYPE_SUPPORT_PKG_EXPORT
const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1();
```

descriptor 中可选的 `ibmw_schema_support_package_v1_t` 保存
`ibmw_schema_support_package_v1_t`，其中保存不可变的
`ibmw_schema_support_v1_t` 描述符。两个结构均以 `abi_version` 和
`struct_size` 开头；消费者读取必选字段前，必须检查版本，并确认
`struct_size` 不小于相应的 `*_V1_REQUIRED_SIZE`。未来 ABI 修订可以在不改变
既有前缀的情况下追加字段。字符串视图和 schema 字节由包持有，生命周期持续
到包卸载。V1 使用规范化的小写 serialization/schema encoding，并对原始 schema
字节计算 `fnv1a64:<16 位小写十六进制>`；hash 仅用于加速查找，不能作为无冲突
标识。必选字符串或 schema 数据为空时，描述符无效。

---

## Driver-Slot ABI 与 Marker 字典

driver-slot ABI 与 marker 公共字典是 user-region 与 transport 解耦的
核心 API 表面。本节给出可直接引用的头文件、宏、与符号。

### `src/api/c/driver_slot.h` —— Slot ID 入口

```c
typedef uint64_t ibmw_driver_slot_t;

// 编译期稳定哈希：driver 名 → slot id
#define IBMW_DRIVER_ID(name)        /* constexpr ibmw_driver_slot_t */
#define IBMW_DECLARE_DRIVER_SLOT(name)  /* 在 Driver 类内注入 kStableDriverId */
```

每个 `*TransportDriver` 子类（local / tcp / http / udp / dds / mqtt /
ros2 / iceoryx / zenoh / shm，共 10 个）通过 `IBMW_DECLARE_DRIVER_SLOT(...)` 在
header 内声明一个稳定 slot id。该 id 进入 Engine 在 `Start()` 阶段调用的
广播链路：`engine.cc::Start()` → `module_bus.cc::BroadcastDriverSlot` →
`package_loader.cc::PackageHandle::SetDriverSlot`。

### `src/api/pkg/pkg_main.h` —— Package ABI

```c
typedef struct {
    /* ... module 入口 ... */
    void (*set_driver_slot)(const char* module_name,
                            ibmw_driver_slot_t slot_id,
                            void* driver_ptr);
} ibmw_pkg_entry_t;
```

`IBMW_PKG_MAIN(...)` 宏在 `src/api/pkg/pkg_main.h` 中导出
入口符号 `IbmwPkgEntry`（typed slot）。该符号在每一个 pkg `.so` 中都通过
`nm -D` 可见。

### `src/api/cpp/module_base.h` —— `IBMW_USE_DRIVER` / `IBMW_USE_DRIVERS`

```cpp
// 单 driver 注入
#define IBMW_USE_DRIVER(DriverT, member_ptr)

// 多 driver 注入（变参）
#define IBMW_USE_DRIVERS(...)                  // 传入若干个 IBMW_DRIVER_ENTRY
#define IBMW_DRIVER_ENTRY(DriverT, member_ptr) // 单个 entry，bool-valued lambda
```

两者均生成对 `Module::SetDriverSlot(slot_id, driver_ptr)` 的 override。
`IBMW_USE_DRIVERS` 内部展开为 `bool results[] = { __VA_ARGS__ };` 数组
形式以避开 comma-operator 陷阱（每个 entry 已经是 `bool` 值）。

约束：
- 每个 `DriverT` 必须发布 `static constexpr const char* kStableDriverId`
  （由 `IBMW_DECLARE_DRIVER_SLOT` 自动注入）
- `member_ptr` 必须是 `DriverT*` 可赋值的成员
- 至少需要一个 `IBMW_DRIVER_ENTRY`，零参数不合法（base-class 默认即 no-op）

### `src/api/cpp/marker.h` —— Marker Grammar 公共字典

```cpp
namespace ibmw::api::cpp::marker {

// 5 类非 RPC marker（user-region / abi / ...）+ 1 类 RPC marker
inline constexpr const char kUserRegionBegin[]   = "...";
inline constexpr const char kUserRegionEnd[]     = "...";
// ... 完整 6 种 token 序列，详见头文件
}  // namespace ibmw::api::cpp::marker
```

inline-constexpr 字典，是 ibmw-gen 与人工代码之间字节序列的唯一来源。

> 任何在 user-region 手写引用 marker 的代码（典型场景：手写 sample
> 模拟 ibmw-gen 输出）必须 `#include "cpp/marker.h"` 并使用 `constexpr`
> 串。硬编码 token 会破坏 ibmw-gen 与 in-tree regen 源码的字节等价。

### Driver Slot 调用顺序图

```
Engine::Start()
    │
    ▼
ModuleBus::BroadcastDriverSlot(slot_id, driver_ptr)
    │  for each PackageHandle:
    ▼
PackageHandle::SetDriverSlot(module_name, slot_id, driver_ptr)
    │  -> IbmwPkgEntry.set_driver_slot
    ▼
Module::SetDriverSlot(slot_id, driver_ptr)        // IBMW_USE_DRIVER(S) 生成的 override
    │  fold over IBMW_DRIVER_ENTRY 列表
    │  if (slot_id == DriverT::kStableDriverId) {
    │      member = static_cast<DriverT*>(driver_ptr);
    │  }
```

---

## 架构图

```
+-------------------------------------------------------------------+
|  用户 Module 代码                                                   |
|  （继承 ibmw::Module、使用 ibmw::context::Context）                 |
+-------------------------------------------------------------------+
|  C++ SDK 层 (ibmw::*)                                              |
|  Runtime, Context, OpPub/Sub/Cli/Srv, res::Publisher/Client...     |
|  协程：co::Task<T>, Schedule, SyncWait, AsyncScope                  |
|  工具：Function<Ops>, BufferArray, TypeSupportRef                   |
+-------------------------------------------------------------------+
|  C ABI 层 (ibmw_*_base_t vtables)                                  |
|  ibmw_core_base_t, ibmw_module_base_t, ibmw_executor_base_t,       |
|  ibmw_transport_*_base_t, ibmw_service_*_base_t, etc.              |
+-------------------------------------------------------------------+
|  Engine Core + Extension                                           |
|  ExtensionBase、transport/RPC driver 以 dynlib 形式存在              |
+-------------------------------------------------------------------+
|  Pkg ABI / Type Support Pkg ABI                                    |
|  module 与 type support package 的 IbmwDynlib* 入口点                |
+-------------------------------------------------------------------+
```

**关键设计模式**：

- **C ABI 桥接**：每个 C++ `*Ref` 类都包装一个 `*_base_t` vtable
  struct，从而跨共享库边界保持 ABI 稳定。
- **Builder 模式**：`OpPub`/`OpSub`/`OpCli`/`OpSrv` 提供资源注册的
  fluent API。
- **四种 RPC 模式**：Sync、Async、Future、Coroutine 访问模式。
- **协程抽象**：`ibmw::co` 封装 libunifex 或 stdexec。
- **SBO Function**：`ibmw::util::Function<Ops>` 用 small-buffer 优化
  + C 兼容 ops vtable 实现跨库类型擦除。
- **PMR context**：Transport 与 RPC context 用 polymorphic memory
  resource 实现 small-buffer 优化的元数据存储。
