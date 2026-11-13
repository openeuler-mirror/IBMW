# IBMW 开发者指南

本指南介绍 IBMW 的构建系统、代码结构，以及如何开发 Module 与 Extension。

## 目录

1. [项目结构](#项目结构)
2. [构建系统](#构建系统)
3. [编写 Module](#编写-module)
4. [编写 Extension](#编写-extension)
5. [类型系统](#类型系统)
6. [Transport 开发](#transport-开发)
7. [RPC 开发](#rpc-开发)
8. [协程支持](#协程支持)
9. [日志](#日志)
10. [红线规则与多 Driver 模块](#红线规则与多-driver-模块)
11. [测试](#测试)

---

## 项目结构

```
ibmw/
  CMakeLists.txt          # 顶层构建配置（332 行）
  VERSION                 # 内容为 "0.1.0"
  cmake/                  # 30 个 CMake 模块
  |  GetFastDDS.cmake     #   FastDDS 依赖
  |  GetAsio.cmake        #   Asio 依赖
  |  DdsIdlGenCode.cmake  #   IDL 代码生成
  |  ...
  src/
    base/                 # 基础工具
    |  log_util.h         #   SimpleLogger、SimpleAsyncLogger
    |  string_util.h      #   FormatTree、字符串辅助
    |  ...
    core/                 # 核心运行时引擎
    |  engine/            #   Engine 子系统实现
    |  |  engine.h/cc     #     中央协调器（生命周期、hook）
    |  |  executor/       #     ExecutorBus + 5 种 Executor 类型
    |  |  extension/      #     ExtensionBus（动态库加载）
    |  |  logger/         #     LoggingSubsystem + formatter + 2 个 driver
    |  |  module/         #     ModuleBus、ConfigSubsystem、ParameterStore、MemoryBus
    |  |  service/        #     ServiceDispatcher（RPC 路由）
    |  |  transport/      #     TransportBus（pub/sub 路由）
    |  main/              #   ibmw 二进制入口
    |  |  main.cc         #     CLI 解析、Engine 生命周期
    |  extension_api/     #   Extension 开发的公共 API
    api/                  # Module 开发的公共 API
    |  c/                 #   C ABI 层（18 个头文件）
    |  cpp/               #   C++ SDK（35+ 个头文件）
    |  pkg/               #   Pkg 入口宏
    |  type_support/      #   Type Support Pkg ABI
    extensions/           # Extension 实现
    |  transport/         #   Transport 后端
    |  |  dds_ext/        #     FastDDS（transport + RPC）
    |  |  net_ext/        #     HTTP/TCP/UDP
    |  |  mqtt_ext/       #     MQTT
    |  |  ros2_ext/       #     ROS2 **(已废弃)**
    |  |  iceoryx_ext/    #     Iceoryx（共享内存）
    |  |  shm_ext/        #     自研共享内存（selfdev/shm，无需 RouDi）
    |  |  zenoh_ext/      #     Zenoh
    |  rpc/
    |  |  grpc_ext/       #     gRPC
    |  observability/     #     Log Control、Topic Logger、OpenTelemetry
    |  management/        #     Parameter、Time Manipulator、Service Introspection、
    |  |                  #     Record/Playback
    |  utility/           #     Echo、Proxy
    schema/               # 类型定义
    |  dds_idl/           #   DDS IDL 与生成代码
    |  ros2/              #   ROS2 msg/srv 定义
    |  extensions/        #   Extension 专属类型（dds_ext_proto、ros2_ext_proto **(已废弃)**）
    samples/cpp/          # 示例应用与验证样本
    tools/                # CLI 工具与代码生成器
```

---

## 构建系统

### CMake 架构

顶层 `CMakeLists.txt` 定义所有构建选项，并通过 `cmake/` 下的模块编排
依赖拉取。

关键 CMake 模块：

| 模块 | 用途 |
|------|------|
| `GetAsio.cmake` | 拉取 Asio（header-only） |
| `GetFastDDS.cmake` | 拉取/查找 FastDDS + FastCDR |
| `GetYamlCpp.cmake` | 拉取 yaml-cpp |
| `GetTbb.cmake` | 拉取 Intel TBB |
| `GetFmt.cmake` | 拉取 fmt 库 |
| `GetNlohmannJson.cmake` | 拉取 nlohmann/json |
| `GetBoost.cmake` | 拉取 Boost（net/grpc extension 使用） |
| `DdsIdlGenCode.cmake` | 从 DDS IDL 文件生成 FastCDR 代码 |
| `GetGTest.cmake` | 拉取 Google Test |

### 添加新依赖

1. 创建 `cmake/GetMyDep.cmake`：
```cmake
include(FetchContent)
FetchContent_Declare(
  mydep
  GIT_REPOSITORY https://github.com/example/mydep.git
  GIT_TAG v1.0.0
)
FetchContent_MakeAvailable(mydep)
```

2. 在相关 `CMakeLists.txt` 中引入：
```cmake
include(${CMAKE_SOURCE_DIR}/cmake/GetMyDep.cmake)
```

### 构建配置

> **OHOS 交叉构建提示：** 如果你要在小艺环境下做 OHOS ARM64 Humble compile-only
> 验证，请先看 [OHOS 小艺 Humble 交叉构建指南](../guides/ohos-xiaoyi-humble-cross-build-guide.zh.md)。
> 那份文档明确区分了最小 configure、DDS source-prefix、rosidl compile-only
> 三条路径，并说明当前**不**覆盖板端运行、DDS discovery、`rclcpp` 或
> `IBMW_BUILD_WITH_ROS2=ON`。

```bash
# Debug 构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Release 含全部 extension（推荐：使用 CMake preset）
cmake --preset full
cmake --build build -j$(nproc)

# 或手动：
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_NET_EXTENSION=ON \
  -DIBMW_BUILD_GRPC_EXTENSION=ON \
  -DIBMW_BUILD_MQTT_EXTENSION=ON \
  -DIBMW_BUILD_ECHO_EXTENSION=ON \
  -DIBMW_BUILD_PROXY_EXTENSION=ON \
  -DIBMW_BUILD_PARAMETER_EXTENSION=ON \
  -DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON \
  -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON \
  -DIBMW_BUILD_TIME_MANIPULATOR_EXTENSION=ON \
  -DIBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION=ON \
  -DIBMW_BUILD_TOPIC_LOGGER_EXTENSION=ON
```

### 清理构建缓存

如目录结构发生变化，重建前先清理 CMake 缓存：

```bash
cd build && rm -rf CMakeCache.txt CMakeFiles
```

---

## 编写 Module

> **提示：** 你可以避免手写样板代码。运行
> `ibmw-gen my_project.ibmw.yaml -o my_project` 即可从单个定义文件生成
> Module 骨架、CMake 规则与 YAML 配置。
> 详见 [ibmw-gen 参考文档](../../src/tools/ibmw_gen/README.zh.md)。

### Module 生命周期

每个 IBMW Module 都是一个继承自 `ibmw::Module` 的 C++ 类，需实现四个公开生命周期方法：

```cpp
#include <ibmw/cpp/core.h>
#include <ibmw/cpp/module_base.h>

class MyModule : public ibmw::Module {
 public:
  // 返回 Module 元数据
  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{
        .module_name = "MyModule",
        .major_version = 1, .minor_version = 0,
        .patch_version = 0, .build_version = 0,
        .author = "My Team",
        .description = "Example module",
    };
  }

  // 加载后调用 -- 注册类型、获取 handle
  bool Setup(ibmw::Runtime runtime) override {
    core_ = runtime;
    // 获取子系统 handle
    auto executor_bus = core_.GetExecutor();
    auto transport = core_.GetTransport();
    auto logger = core_.GetLogger();
    auto params = core_.GetParams();
    auto configurator = core_.GetConfigurator();
    auto service = core_.GetService();
    return true;
  }

  // 所有 Module 加载完毕后调用 -- 开始工作
  bool Run() override {
    MW_INFO("Module started");
    return true;
  }

  // 关闭时调用 -- 清理资源
  void Stop() override {
    MW_INFO("Module stopping");
  }

 private:
  ibmw::Runtime core_;
};
```

### Module 打包（Pkg 模式）

要让 `ibmw` 运行时能将 Module 作为共享库加载，需创建一个 Pkg 入口：

```cpp
// pkg_main.cc
#include <ibmw/pkg/pkg_macro.h>
#include "my_module.h"

static const auto kModuleArray = std::array{
    std::make_tuple("MyModule",
                    []() -> ibmw::Module* { return new MyModule(); }),
};

IBMW_PKG_MAIN(kModuleArray)
```

构建为共享库：
```cmake
find_package(ibmw CONFIG REQUIRED)

add_library(my_module_pkg SHARED
  my_module.cc
  pkg_main.cc
)
target_link_libraries(my_module_pkg PRIVATE ibmw::api::pkg)
```

这里的头文件是安装后的 public header，使用 `<ibmw/...>` 形式引用；不要依赖
源码树中的相对路径或旧的聚合头文件。`ibmw::api::pkg` 是 Pkg 入口所需的
安装态 CMake target。

### Module 作为独立 App（App 模式）

```cpp
// main.cc
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include <ibmw/engine/engine.h>
#include "my_module.h"

using ibmw::runtime::core::Engine;

volatile std::sig_atomic_t stop_requested = 0;

void SignalHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    stop_requested = 1;
    return;
  }
  std::raise(sig);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml>\n";
    return 1;
  }
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  Engine engine;

  // 独立 App 直接注册栈上 Module；Engine 持有其 NativeHandle。
  MyModule module;
  engine.GetModuleBus().RegisterModule(module.NativeHandle());

  Engine::Options options;
  options.cfg_file_path = argv[1];
  engine.Initialize(options);
  auto running = engine.AsyncStart();
  while (!stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  engine.Shutdown();
  running.wait();
  return 0;
}
```

App 目标应链接 `ibmw::core::engine`。主函数需要为 `SIGINT` 和 `SIGTERM`
注册 handler，并按 `Initialize`、`AsyncStart`、正常线程中的 `Shutdown` 和
future wait 管理 Engine 生命周期。不要在 POSIX signal handler 中直接调用
可能加锁、分配内存或等待线程的 runtime 方法。

### 访问每个 Module 的配置

Module 可读取自己的 YAML 配置段：

```yaml
# 在 YAML 配置文件中
MyModule:
  sensor_rate: 100
  topic_name: /sensor_data
```

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  auto config_path = core_.GetConfigurator().GetConfigFilePath();
  // 用 yaml-cpp 或其他 YAML 库解析 config_path
  return true;
}
```

### Transport：发布消息

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  publisher_ = core_.GetTransport().GetPublisher("my_topic");
  ibmw::transport::RegisterSendType<MyMessageType>(publisher_);
  return true;
}

bool Run() override {
  auto executor = core_.GetExecutor().GetExecutor("work_pool");
  executor.Dispatch([this]() {
    MyMessageType msg;
    msg.data = "Hello";
    ibmw::transport::Send(publisher_, msg);
  });
  return true;
}
```

### Transport：订阅消息

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  auto subscriber = core_.GetTransport().GetSubscriber("my_topic");
  ibmw::transport::OnMessage<MyMessageType>(subscriber,
    [](const MyMessageType& msg) {
      MW_INFO("Received: {}", msg.data);
    });
  return true;
}
```

### RPC：注册 Server

```cpp
bool Setup(ibmw::Runtime runtime) override {
  core_ = runtime;
  auto service = core_.GetService();

  // 注册 RPC server function
  RegisterMyServiceServerFunc(service);  // 由 IDL 生成

  // 设置 handler
  MyServiceSyncAccess access(service, "/my_service/Query");
  access.RegisterHandler([](const MyRequest& req, MyResponse& rsp) {
    rsp.result = process(req.input);
    return ibmw::rpc::Status();  // OK
  });

  return true;
}
```

### RPC：发起 Client 调用

```cpp
// 同步
MyServiceSyncAccess access(service, "/my_service/Query");
MyRequest req; req.input = 42;
MyResponse rsp;
auto ctx = ibmw::rpc::Context(IBMW_RPC_CLIENT_CONTEXT);
ctx.SetTimeoutNs(3'000'000'000);  // 3 秒
auto status = access.Invoke(ctx.GetContextRef(), req, rsp);
if (status.OK()) {
  MW_INFO("Result: {}", rsp.result);
}

// 协程
co::Task<void> MyCoroutine() {
  MyServiceCoAccess access(service, "/my_service/Query", executor);
  MyRequest req; req.input = 42;
  MyResponse rsp;
  auto ctx = ibmw::rpc::Context(IBMW_RPC_CLIENT_CONTEXT);
  auto status = co_await access.Invoke(ctx.GetContextRef(), req, rsp);
}
```

### 使用高层 Context API

Context API 提供 builder 模式接口，自动管理资源生命周期：

```cpp
#include "cpp/context/context.h"

class MyModule : public ibmw::Module {
  ibmw::context::Context ctx_;
  ibmw::context::res::Publisher<MyMsg> pub_;
  ibmw::context::res::Subscriber<MyMsg> sub_;

  bool Setup(ibmw::Runtime runtime) override {
    ctx_ = ibmw::context::Context(runtime, "work_pool");

    // 创建 publisher
    ctx_.Pub<MyMsg>("my_topic").Use(pub_).Build();

    // 创建带回调的 subscriber
    ctx_.Sub<MyMsg>("my_topic")
        .Use(sub_)
        .SetCallback([](const MyMsg& msg) {
          // 处理消息
        })
        .Build();

    return true;
  }

  bool Run() override {
    MyMsg msg;
    msg.data = "Hello";
    pub_.Publish(msg);
    return true;
  }

  void Stop() override {
    ctx_.Shutdown();
  }
};
```

---

## 编写 Extension

Extension 是实现 `ExtensionBase` 的共享库。它们在运行时加载，可以
注册 transport driver、RPC driver、log 后端等。

### Extension 结构

```cpp
// my_extension.h
#include <ibmw/engine/extension/extension_base.h>

class MyExtension : public ibmw::ExtensionBase {
 public:
  std::string_view Name() const noexcept override {
    return "my_extension";
  }

  bool OnInit(ibmw::runtime::core::Engine* core_ptr) override {
    // 访问 engine 子系统
    // 注册 transport/RPC driver
    // 从 YAML 读取 extension 选项
    return true;
  }

  void OnShutdown() override {
    // 清理资源
  }

  std::list<std::pair<std::string, std::string>>
  GenInitializationReport() const override {
    return {{"key", "value"}};  // 显示在启动日志中
  }
};
```

### Extension 入口

```cpp
// my_extension.cc，在全局作用域
#include "my_extension.h"

IBMW_EXT_ENTRY(MyExtension)
```

### Extension 日志

使用 extension logger 宏实现按 extension 维度的日志：

```cpp
// global.h
#include "extension_logger.h"
IBMW_DECLARE_EXTENSION_LOGGER(my_ext)

// global.cc
IBMW_DEFINE_EXTENSION_LOGGER(my_ext)
```

### Extension CMake

```cmake
find_package(ibmw CONFIG REQUIRED)

add_library(my_extension SHARED
  my_extension.cc
)
target_link_libraries(my_extension PRIVATE
  ibmw::core::extension_api
  ibmw::core::engine
)
```

Extension 必须与 IBMW runtime 使用相同的目标架构和依赖 provider 构建。其共享库
由 runtime 加载，不能把 Extension 入口当作普通 managed 业务 API。

### Extension YAML 配置

Extension 从 YAML 配置接收选项：

```yaml
ibmw:
  extensions:
    items:
      - name: my_extension
        path: ./lib/libmy_extension.so
        options:
          my_option: value
```

`options` 节点通过 engine 的 configuration 子系统传给 extension 的
`Initialize()` 方法。

---

## 类型系统

IBMW 以 DDS IDL 作为主要的类型定义语言，使用 FastCDR 进行序列化。
不依赖 protobuf。

### 用 DDS IDL 定义类型

创建一个 IDL 文件：

```idl
// my_types.idl
module protocols {
  struct SensorData {
    uint64 timestamp;
    float temperature;
    float humidity;
    string sensor_id;
  };
};
```

### 代码生成

`DdsIdlGenCode.cmake` 模块生成 FastCDR 序列化代码：

```cmake
include(DdsIdlGenCode)
ibmw_dds_idl_gen(
  TARGET my_types
  IDL_FILES my_types.idl
)
```

生成内容：
- `my_types.h` -- 类型定义
- `my_types_cdr.h/.cc` -- FastCDR 序列化/反序列化函数
- `my_types_dds_types.h` -- DDS TopicDataType 适配器

### 手写类型（不走代码生成）

手写 DDS typesupport 仅供 Extension 或 direct DDS 开发使用，属于 transport
实现边界内的能力，不是普通业务 Module 的公共 API。普通用户应优先使用
`DdsIdlGenCode.cmake` 生成类型、序列化代码和 TypeSupport；只有确有
transport-specific 需求时才考虑手写实现。

direct DDS 目标必须链接 `ibmw::extensions::dds_types`；下方安装态头文件路径
是该 extension-facing target 对应的受支持入口。不要给业务目标手工添加
`src/extensions/...` 源码树 include 路径。

也可以手动定义类型：

```cpp
#include <ibmw/dds_types/util/dds_type_support.h>

struct HelloMsg {
  uint64_t id;
  std::string message;
  int64_t timestamp;

  IBMW_DDS_TYPE_NAME("HelloMsg")

  void serialize(eprosima::fastcdr::Cdr& cdr) const {
    cdr << id << message << timestamp;
  }

  void deserialize(eprosima::fastcdr::Cdr& cdr) {
    cdr >> id >> message >> timestamp;
  }
};

// 注册 FastCDR v1 自由函数
template<>
size_t calculate_serialized_size(
    eprosima::fastcdr::CdrSizeCalculator& calc,
    const HelloMsg& msg, size_t& current_alignment);

template<>
void serialize(eprosima::fastcdr::Cdr& cdr, const HelloMsg& msg);

template<>
void deserialize(eprosima::fastcdr::Cdr& cdr, HelloMsg& msg);
```

### JSON 类型

对于 management/service 类型，IBMW 通过 nlohmann/json 支持 JSON 序列化：

```cpp
#include "cpp/util/json_type_support.h"

struct MyConfig {
  static constexpr char kTypeName[] = "my_app::MyConfig";
  std::string name;
  int value;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(MyConfig, name, value)
};

// 获取 type support
const auto* ts = ibmw::json::GetJsonMessageTypeSupport<MyConfig>();
```

### Type Support Pkg

为支持运行时类型发现（被 Echo、Proxy、Record/Playback extension 使用），
需要创建一个 Type Support Pkg：

```cpp
#include "ibmw_type_support_abi.h"

static const ibmw_type_support_base_t* kTypeSupportArray[] = {
    GetSensorDataTypeSupport(),
    GetImageTypeSupport(),
};

const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  static const ibmw_type_support_package_v1_t kPackage{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2,
      .struct_size = sizeof(ibmw_type_support_package_v1_t),
      .entry_count = std::size(kTypeSupportArray),
      .entries = kTypeSupportArray,
      .schema_support = nullptr,
      .reserved = {}};
  return &kPackage;
}
```

---

## Transport 开发

### 架构

Transport 子系统采用基于 driver 的架构：

```
TransportBus
  |-- TransportDriver (local)
  |-- TransportDriver (dds)
  |-- TransportDriver (mqtt)
  |-- ...
```

每个 driver 实现：
- Publisher 注册与消息发送
- Subscriber 注册与消息投递
- Topic 到 driver 的路由（通过 YAML 配置）

### 路由

Transport 路由基于正则。Publisher 和 Subscriber 通过 `enable_drivers`
匹配到 driver：

```yaml
messaging:
  publishers:
    - topic: "/sensor/.*"
      enable_drivers: [dds]
    - topic: "/internal/.*"
      enable_drivers: [local]
  subscribers:
    - topic: ".*"
      enable_drivers: [local, dds]
```

### Filter

Transport filter 在发送/接收前后拦截消息。通过路由规则中的
`enable_filters` 配置。

---

## RPC 开发

### 架构

RPC 子系统的架构与 transport 类似：

```
ServiceDispatcher
  |-- RpcDriver (local)       -- 进程内，无网络
  |-- RpcDriver (dds)         -- 纯 DDS，与 ROS2 wire 兼容
  |-- RpcDriver (grpc)        -- gRPC 远程
  |-- RpcDriver (http)        -- HTTP 远程
  |-- ...
```

`dds` driver 由 DDS extension 注册。它使用**原生 `.srv` 模式**：业务请求/
响应类型直接由 FastDDS 通过 gencode 提供的 TypeSupport
（`info.req_type_support_ref.CustomTypeSupportPtr()`）进行序列化/反序列化。
不存在 envelope wrapper 类型。对于匹配且已验证的 ROS2 release、FastDDS/
Fast-CDR profile、生成类型、QoS 与请求/响应关联契约，其 CDR payload 设计为与
ROS2 service 一致。这不是跨版本、跨 vendor 的普遍兼容保证；每个 service 产品
仍需具备 typed 双向验收证据。

该 driver 仅使用 FastDDS 公共 API 实现 ROS2 Service wire 协议
（WriteParams/SampleInfo 关联、`rq/`/`rr/` topic 命名、`dds_::` 类型名中缀），
不依赖 rcl/rclcpp。当 `ros2_compatible: true` 时，IBMW service 可被标准
ROS2 节点发现和调用（`ros2 service call`、`ros2 service list`）。

关键实现细节：
- TypeSupport 由 gencode 注入，而非在 driver 中硬编码
- DDS 类型名遵循 ROS2 约定：`<pkg>::srv::dds_::<Name>_Request_`
- `dds_rpc_types.h`（envelope wrapper）已被移除
- ibmw-gen 中的 `srv:` 字段会从 `.srv` 包路径自动推导出
  `req_dds_type_name`、`rsp_dds_type_name` 与 `ros2_service_name`

### 四种调用模式

| 模式 | Access 类 | 阻塞 | 返回 |
|------|----------|------|------|
| 同步 | `SyncAccessBase<Req, Rsp>` | 是 | `rpc::Status` |
| 异步回调 | `AsyncAccessBase<Req, Rsp>` | 否 | 通过回调 |
| Future | `FutureAccessBase<Req, Rsp>` | 否 | `std::future<rpc::Status>` |
| 协程 | `CoAccessBase<Req, Rsp>` | 否 | `co::Task<rpc::Status>` |

异步和 future 模式按地址把 `req`、`rsp` 交给 provider。使用引用版本时，调用方必须保证两者存活到回调执行或 future 就绪。生成的 `AsyncAccess`、`FutureAccess` 还接受 `std::shared_ptr<const Req>` 和 `std::shared_ptr<Rsp>`，这组重载会把两个对象保活到调用完成。`req` 或 `rsp` 为空时以 `IBMW_RPC_STATUS_CLI_UNKNOWN` 完成。

### RPC Filter

基于协程的 RPC filter 拦截调用以实现横切关注点：

```cpp
class TimingFilter : public ibmw::CoRpcFilter {
  co::Task<rpc::Status> Handle(CoRpcHandle& handle,
                                CoFilterBus& next) override {
    auto start = std::chrono::steady_clock::now();
    auto status = co_await next.Next(handle);
    auto elapsed = std::chrono::steady_clock::now() - start;
    MW_INFO("RPC took {}us",
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    co_return status;
  }
};
```

在 server 操作上注册 filter：

```cpp
ctx_.Srv<Req, Rsp>("/my_service/Query")
    .Use(srv_)
    .SetCallback(handler)
    .AddFilter(std::make_shared<TimingFilter>())
    .Build();
```

---

## 协程支持

IBMW 通过 `ibmw::co` 命名空间提供 C++20 协程工具：

### 调度

```cpp
// 在指定 executor 上调度任务
co_await co::Schedule(executor);

// 延迟一段时间后调度
co_await co::ScheduleAfter(executor, std::chrono::seconds(1));

// 在指定时间点调度
co_await co::ScheduleAt(executor, time_point);
```

### Task

```cpp
co::Task<int> ComputeAsync() {
  co_await co::Schedule(executor);
  co_return 42;
}
```

### AsyncScope（结构化并发）

```cpp
co::AsyncScope scope;

// 派生多个并发任务
scope.spawn(co::On(scheduler, Task1()));
scope.spawn(co::On(scheduler, Task2()));

// 等待所有任务完成
co_await scope.join();
```

### SyncWait（阻塞）

```cpp
auto result = co::SyncWait(ComputeAsync());
```

---

## 日志

### 日志宏

```cpp
MW_TRACE("Trace message: {}", value);
MW_DEBUG("Debug message: {}", value);
MW_INFO("Info message: {}", value);
MW_WARN("Warning: {}", value);
MW_ERROR("Error: {}", value);
MW_FATAL("Fatal: {}", value);
```

### 日志级别

| 级别 | 数值 | 说明 |
|------|------|------|
| Trace | 0 | 详细跟踪 |
| Debug | 1 | 调试信息 |
| Info | 2 | 一般信息（默认） |
| Warn | 3 | 警告 |
| Error | 4 | 错误 |
| Fatal | 5 | 致命错误 |
| Off | 6 | 关闭日志 |

### 日志格式 pattern

默认 pattern 为：`%l %c.%f [%n] %v (%G:%R)`

| 占位符 | 说明 |
|--------|------|
| `%l` | 日志级别 |
| `%c` | 日期与时间 |
| `%f` | 微秒 |
| `%n` | Module 名 |
| `%v` | 日志消息 |
| `%G` | 短文件名 |
| `%R` | 行号 |
| `%g` | 完整文件路径 |
| `%F` | 函数名 |
| `%t` | 线程 ID |

---

## 多 Driver 模块

IBMW 围绕 transport 解耦设立了一组代码质量红线，由
`scripts/check_red_lines.sh` 进行扫描，详细规则面向贡献者的部分见
`skills/transport_decoupling.md`。

### `IBMW_USE_DRIVER` vs `IBMW_USE_DRIVERS`

`IBMW_USE_DRIVER`（单 driver）与 `IBMW_USE_DRIVERS`（多 driver）是受限的
escape hatch。它们只允许用于明确命名的 `direct_<transport>_*` 模块，例如
`direct_dds_publisher`、`direct_iceoryx_zero_copy`，以及 benchmark 或确实
需要 transport-specific zero-copy 的实现。普通 managed Module 禁止持有
concrete driver，也不得用这些宏绕过公共 Transport API。

普通业务代码应只依赖安装态 public API（例如 `ibmw::api::cpp` 提供的
Publisher/Subscriber 等抽象），其 CMake target 使用 `ibmw::api::cpp`；
只有 direct/Extension 代码才可以依赖具体 transport 的 direct target（例如
`ibmw::core::engine` 及相应 transport extension 导出的 target）。这些 direct
target 不属于普通业务公共 API。

两者展开后都生成一个 `Module::SetDriverSlot(slot_id, driver_ptr)` override，
由 Engine 在 `Start()` 阶段广播 driver-slot id 时回调。

**受限的单 driver 示例（目录名必须体现 direct 边界）：**
```cpp
class DirectDdsPublisher : public ibmw::Module {
 private:
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  IBMW_USE_DRIVER(ibmw::extensions::dds_extension::DdsTransportDriver, dds_drv_)
};
```

**受限的多 driver 示例（仅 direct/benchmark/zero-copy）：**
```cpp
class DirectTransportBenchmark : public ibmw::Module {
 private:
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* iox_drv_ = nullptr;
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver, dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxTransportDriver, iox_drv_))
};
```

`IBMW_DRIVER_ENTRY` 在编译期把 `DriverT::kStableDriverId`（由
`IBMW_DECLARE_DRIVER_SLOT` 注入的稳定标识）与广播到的 `slot_id` 比对，
命中即 `static_cast<DriverT*>` 赋值。任何 driver 名拼写错误都会编译期报错。

> 完整端到端样例见 `src/samples/cpp/multi_driver_demo/`，宏定义见
> `src/api/cpp/module_base.h`，slot id 入口见 `src/api/c/driver_slot.h`。

`multi_driver_demo` 及其他 `direct_<transport>_*` 样例是边界验证或性能用途，
不能作为普通 managed Module 的依赖或 API 模板。公共 API 与 direct target
的边界必须在目录名、目标依赖和代码审阅中保持清晰。

---

## 测试

### 运行测试

```bash
# 红线检查：report 用于查看既有问题，baseline 仅阻止新增问题
bash scripts/check_red_lines.sh --report
bash scripts/check_red_lines.sh --baseline scripts/baseline_red_lines.txt

cmake -B build -DIBMW_BUILD_TESTS=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

红线脚本基于静态文本扫描，只是辅助门禁，不是架构合规证明。脚本配置的扫描
范围可能不覆盖新增 sample 或自定义目录；代码审阅仍需检查依赖方向、公共头文件
和 direct escape hatch 的命名与边界。

### 测试示例 demo

大多数 demo 不会自动退出。自动化测试中使用 `timeout`：

```bash
# 普通 demo（响应 SIGINT）
timeout -s INT 5 ./bin/ibmw run ./cfg/my_config.yaml

# DDS demo（关闭较慢，可能需要 SIGKILL）
timeout -s KILL 10 ./dds_native_app ./cfg/dds_native_cfg.yaml
```

退出码：0/124/130 = 成功（124=超时，130=SIGINT）。

### 编写单元测试

```cpp
#include <gtest/gtest.h>
#include "my_module.h"

TEST(MyModuleTest, BasicTest) {
  // 在不依赖 IBMW 运行时的情况下测试 module 逻辑
  MyModule module;
  // ...
}
```
