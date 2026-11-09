# IBMW 单元与功能回归

独立维护从 topic CLI 开始增加的单元回归和真实功能回归。现有功能目录和
`scripts/tests/` 中的测试保持原位；本目录不迁入、包含或复制那些测试文件。
单元测试直接编译或链接生产实现；功能测试执行独立 CLI 和业务进程，
通过真实通信及业务侧观测验证功能目的，而不是只检查命令返回成功。

```text
tests/       单元及进程内组件用例
functional/  真实功能场景、进程管理和结果取证
fixtures/    业务测试进程、类型包、生命周期故障进程
support/     单元测试辅助代码及 RPC 替身
```

## 单元测试：构建和运行

在仓库根目录执行，使用项目既有依赖获取方式：

```bash
cmake -S . -B build-diag \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIBMW_BUILD_TESTS=ON \
  -DIBMW_BUILD_RUNTIME=ON \
  -DIBMW_BUILD_PROTOCOLS=OFF \
  -DIBMW_INSTALL=OFF

# 只构建本套件及其生产依赖，不构建整个仓库的测试。
cmake --build build-diag --target ibmw_diag -j4
ctest --test-dir build-diag -L '^ibmw_diag$' --output-on-failure --no-tests=error

# 或者一次完成构建和执行。
cmake --build build-diag --target check_ibmw_diag -j4

# 单独运行某一组；所有目标名称均为 ibmw_diag_<功能>_test。
ctest --test-dir build-diag -R '^ibmw_diag_module_test$' --output-on-failure
```

默认单元配置不需要启用任何传输扩展，也不要求安装 ROS2、启动 DDS 参与者、
MQTT broker 或业务进程。单元入口始终只执行进程内测试。
依赖离线源码可通过项目已有的 `<dependency>_LOCAL_SOURCE` CMake 变量指定。

本目录随 `IBMW_BUILD_TESTS=ON` 且 `IBMW_BUILD_RUNTIME=ON` 接入。
`IBMW_BUILD_CLI_TOOLS` 控制另一个 Python CLI 打包工具，不是本套件的开关。
测试目标不安装进 SDK，也不加入生产 CLI 的 provider 清单。

## 单元覆盖和边界

| 测试组 | 主要断言 | 隔离方式 |
|---|---|---|
| `topic` | 发现结果去重、逻辑/线缆名称、类型与隐藏项；发现域；hz/bw 窗口和时间；发布路由/QoS/TLS 参数；echo 字段与原始字节 | 内存快照、显式时钟、配置解析、内存消息；不发消息 |
| `service` | 目录协议身份去重、边界校验、隐藏服务、gRPC 帧、地址与域编码；各后端调用配置、独立超时、请求转换 | 内存协议与配置；不调用传输 |
| `module` | list/count/runtime 选择；旧协议覆盖不全；info 同名歧义、接口去重、方向与类型；错误及参数预校验 | 替换 `RuntimeRegistry::DiscoverLocalRuntimes`，执行真实命令解析和输出；不访问登记文件 |
| `param` | get/set 请求和值；带点前缀与正则过滤；排序和分组；列表中途失败不能输出部分成功；状态校验、超时及不重试 | 真实命令和协议类型，替换 `ExecuteCall` |
| `log` | 查询范围、等级标准化、setter 请求；空结果与缺失数据；业务/协议错误；CLI 输出和中断码 | 真实命令、客户端和协议类型，替换 `ExecuteCall` |
| `doctor` | topic 存在性与 QoS 的区别；缺失/隐藏/不同域；QoS 方向、精度、未知值、partition/ownership；日志显式基准；模块失败、未完成和不适用阶段 | 直接调用生产诊断规则，输入完全由测试构造 |
| `lifecycle` | 初始化/启动状态、失败原因和异常传播；回调内查询；快照独立性与关闭后保留；禁用模块、app context；协议校验 | 真实 ModuleBus 加内存测试模块；不创建 Engine 或加载业务包 |

以上单元及进程内组件回归，不代表真实传输通信、CLI 动态插件装载、
registry 文件/Unix socket、安装态或性能测试已经通过。
`doctor` 当前验证诊断规则，不替代其完整命令调度及退出码集成测试。
原有 topic/service/param/log 与底层组件测试仍各自保留，按需要独立执行。

## 功能测试：构建和运行

下面是只启用 HTTP/TCP/UDP 的快速入口。合入前的完整验收使用下一节的
`IBMW_DIAG_FULL_FUNCTIONAL_TESTS=ON`，不能把快速入口通过作为完整验收。

需要 Linux/POSIX、Python 3.8+、本机回环网络以及以下三个真实扩展。
使用单独的构建目录；依赖仍按项目已有方式获取，可传入本机依赖源码路径。

```bash
cmake -S . -B build-diag-functional \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIBMW_BUILD_TESTS=ON \
  -DIBMW_BUILD_RUNTIME=ON \
  -DIBMW_BUILD_PROTOCOLS=OFF \
  -DIBMW_INSTALL=OFF \
  -DIBMW_BUILD_NET_EXTENSION=ON \
  -DIBMW_BUILD_PARAMETER_EXTENSION=ON \
  -DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON \
  -DIBMW_DIAG_BUILD_FUNCTIONAL_TESTS=ON

# 构建真实 CLI、运行时夹具及扩展，并执行全部功能场景。
cmake --build build-diag-functional --target check_ibmw_diag_functional -j4

# 已构建后单独运行一组。
ctest --test-dir build-diag-functional -R '^ibmw_diag_topic_functional_test$' \
  --output-on-failure --no-tests=error
```

功能测试使用独立的 `ibmw_diag_functional` 标签；单元测试的 `ibmw_diag`
标签不包含功能场景。启用功能开关却缺少所需扩展时，配置直接报错，
不会用 mock 替代或静默跳过。多配置生成器执行 CTest 时还需传 `-C Debug`。

## 完整功能验收

在已配置 ROS 2 的 Linux 环境运行，需要匹配的 Fast DDS/Fast CDR、Boost、
带共享内存及 unstable API 的 Zenoh C、Iceoryx、Paho MQTT、nghttp2，以及
Python 的 `rclpy`、`std_msgs`、`example_interfaces`、`mosquitto` 和 `openssl`。
使用项目已有的依赖源码/包路径变量；测试不会修改系统服务或复用用户的 broker。

```bash
cmake -S . -B build-diag-full \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIBMW_BUILD_TESTS=ON -DIBMW_BUILD_RUNTIME=ON \
  -DIBMW_BUILD_PROTOCOLS=ON -DIBMW_INSTALL=ON \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_NET_EXTENSION=ON -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_EXTENSION=ON -DIBMW_BUILD_MQTT_EXTENSION=ON \
  -DIBMW_BUILD_ZENOH_EXTENSION=ON -DIBMW_BUILD_GRPC_EXTENSION=ON \
  -DIBMW_BUILD_ICEORYX_EXTENSION=ON \
  -DIBMW_BUILD_PARAMETER_EXTENSION=ON -DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON \
  -DIBMW_DIAG_BUILD_FUNCTIONAL_TESTS=ON \
  -DIBMW_DIAG_FULL_FUNCTIONAL_TESTS=ON
cmake --build build-diag-full --target check_ibmw_diag_functional -j4
```

完整入口要求所有支持的后端实际构建，缺少依赖即失败，不能静默跳过。
以下是验收要求，不是当前通过声明；实现未满足要求时保留失败，不能把失败改成跳过。
除快速入口外，增加以下真实进程验收：

| 功能 | 追加的实际执行路径 |
|---|---|
| topic | DDS、原生 ROS 2、MQTT、Zenoh、Iceoryx 的发布、发现与速率/带宽采集；MQTT/Iceoryx 通过本机登记查询接口 |
| service | DDS、gRPC、MQTT、Zenoh、ROS 2 的 list/type/find 和实际调用；DDS 调用原生 ROS 2 服务 |
| param/log/doctor log | DDS、gRPC、MQTT、Zenoh；保留实际参数读取、日志输出、隔离、错误、退出及重启断言 |
| MQTT TLS | 服务端认证和双向证书认证，覆盖 topic/service/param/log/doctor；service 发现与调用验证缺失或不可信证书的拒绝行为 |
| DDS/ROS 2 QoS | 真实兼容/不兼容端点和消息接收记录，对照 doctor 判断；覆盖可靠性、持久性、deadline、liveliness、租期及深度差异 |
| DDS 专有 QoS | 真实 ownership、partition 和分区通配符；同一发送者的兼容订阅者必须仍能收到消息 |
| 安装部署 | 执行真实安装，移动安装目录后重跑全部功能场景；检查 CLI 和后端依赖不能回退到原构建目录 |

`local` 本身只在当前进程内通信；快速入口已通过独立运行时的实际 local
发布/接收记录和 Runtime Tap 验证。它不是可向其他进程投递的网络后端。
gRPC 仅适用于 RPC，Iceoryx 仅适用于这里的 topic 发布；param/log 不支持
原生 ROS 2 服务协议，不把不支持的组合列为通过或跳过。

安装验收会构建安装规则需要的 SDK/依赖产物，但不会把测试程序安装进正式 SDK。
测试专用进程和类型包只复制到该次临时验收目录。每个场景均保存独立日志和观测数据。
安装后的环境按使用文档设置 `LD_LIBRARY_PATH` 和 ROS 包前缀，指向移动后的安装目录。
测试应在专用测试主机上串行执行；发现已有 RouDi 时明确失败，不关闭用户的实例。
不要在同一构建目录运行测试的同时重新构建或替换动态库。

还可用独立的 Fast DDS 3 构建开启 `IBMW_DIAG_DDS_QOS_TESTS=ON`，单独执行
`ibmw_diag_native_dds_qos_network_test`。该路径不要求将 ROS 2 Humble 的 DDS 2
依赖和 DDS 3 混装；依赖前缀必须与所选 Fast DDS ABI 匹配。

## 功能验收标准

| 场景 | 真实执行路径 | 成功依据 |
|---|---|---|
| topic 发现与观察 | CLI → 本机 registry / Runtime Agent / Runtime Tap → 独立运行时 | list/type/find/info 对应真实模块接口；hz/bw 与发送端时间和字节记录相符；echo 查询不存在的 topic 明确失败 |
| topic 发布 | CLI → HTTP、TCP、UDP → 独立订阅模块 | 三种传输各收到指定值的三条消息，检查业务接收记录，不以“Published”文本为成功依据 |
| service | CLI → 本机/HTTP 服务发现；CLI → HTTP RPC → 业务求和服务 | 实际求和结果；无效输入不发送；业务失败、超时、服务退出不能返回成功；服务端记录证明不重试 |
| param | CLI → HTTP → 参数扩展 → 业务模块 Params API | 模块观察文件确实变为设置值；其他模块不受影响；过滤、字面值、错误和重启行为正确 |
| log | CLI → HTTP → 日志控制扩展 → 业务 Logger API | 每个日志等级实际输出的消息集合正确；其他模块不受影响；无效请求和重启不破坏默认值 |
| module | CLI → 真实登记目录 → 两个真实 managed 模块 | 名称、PID、计数和接口正确；两个运行时同名时要求选择；退出后不再列出 |
| doctor | CLI → 实际 topic / module / log 采集与诊断 | 健康场景正常；真实缺少订阅者、日志等级偏差得到 WARN；严格模式退出 1；无法查询退出 2 |
| 生命周期失败 | 实际 ModuleBus 回调失败 → 实际 Runtime Agent → 独立 doctor CLI | 初始化/启动失败原因与阶段正确；查询不重复执行回调；旧接口不被误报成功 |

生命周期故障夹具特意在 ModuleBus 失败后保留 Agent，以验证记录和查询链路；
它不代表启动失败并已退出的完整 Engine 仍能被查询。正常生命周期场景使用完整 Engine。
消息类型包同时导出 TypeSupport 和 SchemaSupport，后者是 Runtime Tap 的必要条件。
hz/bw 的容差用于允许调度差异并检验单位与实际消息流，不是性能基准。

上表描述快速入口；完整入口还执行前一节的全部后端、QoS、TLS 和安装验收。
功能通过不代表延迟、吞吐量或跨操作系统/跨硬件性能已达标。

每个用例创建私有登记目录、独立业务进程和动态端口，使用就绪条件及有界等待，
退出时回收进程；业务进程无法正常退出也判失败。日志、命令输出、配置及业务观测
保存在 `build-diag-functional/src/tools/ibmw_diag/functional/results/<配置>/<组>/run-*`。
每次运行单独留存证据，不覆盖上一次失败记录；这些运行产物不提交。

## 缺陷处理

测试夹具或断言本身错误时修正测试并复测。若真实功能违反验收目的，保持用例失败，
记录复现命令及业务侧证据；生产修复须关联回归用例，修复后重新构建并验证实际业务效果。

## 维护约定

- 新单元用例放在对应 `tests/*_test.cc`，通过 CTest 的 `ibmw_diag` 和 `unit` 标签筛选；真实功能场景维护在 `functional/`，业务测试程序维护在 `fixtures/`。
- 测试替身只替换外部查询/执行边界，不重新实现待测业务规则。
- RPC 替身记录请求并按顺序消费响应；未预期的调用和未消费的响应都会失败。
- 单元测试使用显式时间、内存数据和固定输入；功能测试自行启动服务并按实际效果等待，不能依赖已有业务进程或历史日志。
- 功能断言只检查稳定契约。修改契约时同步维护用例，不为了通过测试放宽错误判定。
- 不因有本目录就删除原有测试；不从其他目录复制同一套测试注册，避免一次回归重复执行同一用例。

仓库通用规范见 [Testing and Verification](../../../docs/contributing/testing.md)。

## HTTP 边界回归

启用功能测试后，`ibmw_diag_http_response_validation_test` 由真实 HTTP 服务返回
畸形或合法的参数/日志响应，验证六条 CLI 路径的退出码与输出。对应 TypeSupport
字段、整数范围、合法空值及错误响应测试保留在参数/日志原有 service test 目标内，
JSON-only 和 DDS 构建都运行 JSON 校验，DDS 构建继续验证 CDR 往返。

`ibmw_diag_http_discovery_deadline_test` 通过测试专用 `LD_PRELOAD` 对两个固定域名
注入解析延迟；其他主机名仍走系统解析。测试覆盖 HTTP 单源与 registry 混合查询、
IPv4/IPv6、DNS 与读取共享预算、连接/读取超时，以及重复查询后的子进程、文件描述符
和线程回收。生产 DNS helper 随 `discovery` 安装组件放在 provider 同目录；测试夹具
不安装。进程回收依赖操作系统调度，测试允许有限调度余量，不声称硬实时保证。
