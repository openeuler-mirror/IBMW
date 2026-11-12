<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# IBMW 参数命令

`ibmw param` 通过已有的集中式 `ParameterService` 访问运行时参数。
命令采用 `list [module]`、`get <module> <parameter_name>`、
`set <module> <parameter_name> <value>` 的形式，复用 `service call` 的
后端配置、就绪等待、超时和退出流程。它管理 IBMW 自身的字符串参数，
不提供 ROS 2 参数服务协议或 ROS 2 的强类型参数系统。

## 使用

服务端必须加载参数扩展，并将参数服务路由到选定的 RPC 后端。
每次命令必须且只能指定一个后端，即使服务端就在本机：

```bash
# HTTP：省略 module 时，列出所选参数服务内所有模块的参数
ibmw param list --http 127.0.0.1:50080
ibmw param list CarModule --http 127.0.0.1:50080
ibmw param get CarModule speed --http 127.0.0.1:50080
ibmw param set CarModule speed 0.5 --http 127.0.0.1:50080

# 只输出值，便于脚本读取
ibmw param get CarModule speed --hide-type --http 127.0.0.1:50080

# 字符串原样传递；所有选项放在 -- 之前
ibmw param set CarModule speed --http 127.0.0.1:50080 -- -1.5
ibmw param set CarModule description '' --http 127.0.0.1:50080

# 列表过滤：正则从名称开头匹配；前缀按点分隔，允许重复指定
ibmw param list CarModule --filter '^controller\.' --param-type --dds --domain 10
ibmw param list CarModule --param-prefixes controller --dds --domain 10

# 其他后端
ibmw param get CarModule speed --dds --domain 10
ibmw param get CarModule speed --grpc 127.0.0.1:50051
ibmw param get CarModule speed --zenoh --zenoh-domain car
ibmw param get CarModule speed --mqtt tcp://127.0.0.1:1883 --mqtt-server-id car
```

支持 `--dds`、`--http`、`--grpc`、`--mqtt`、`--zenoh`，实际可用性取决于
构建和部署的后端库。DDS 使用 CDR，其他后端默认使用 JSON。
`--transport-library`、`--zenoh-config`、MQTT 的连接/TLS 选项沿用
[`service call`](user-guide.zh.md#调用-service)。参数协议已内置，
无需传请求/响应类型或 `--type-support`，没有 `--ros2`、`--runtime` 或 `--registry`。

## 服务实例和模块名称

默认服务基础名称为 `ibmw.protocols.parameter_extension.ParameterService`。
例如读取参数时，实际方法为：

```text
json:/ibmw.protocols.parameter_extension.ParameterService/Get
请求：{"module_name":"CarModule","parameter_key":"speed"}
```

后端地址、通信域及 `--service` 选择服务实例，请求中的 `module_name`
选择该实例内部的参数表。模块名称必须与运行时注册的名称完全一致，
不自动添加逻辑 namespace，也不从模块名推导另一组服务端点。

多实例共享一个通信域时，为其配置不同的服务基础名称，例如
`car_a/parameters` 和 `car_b/parameters`，并在调用时对应指定：

```bash
ibmw param get CarModule speed --dds --domain 10 --service car_a/parameters
```

`--service` 填服务基础名称，不包含 `json:` 前缀或 `/Get` 等方法后缀。
HTTP/gRPC 还可通过不同主机地址/端口区分实例，MQTT 可使用 `--mqtt-server-id`。
同一可达域中多个实例暴露同名服务时，不保证选中哪一个实例。

`list` 不带模块时调用新增的 `ListModules`，再逐一调用 `List`。
它只列出**所选集中式参数服务**中的参数，不扫描所有设备或所有服务。
结果按模块和参数名称排序；任一调用失败时不输出不完整列表。
旧服务端没有 `ListModules`，需升级才能使用无模块的 `list`。

## 服务端配置

以下内容合并到应用现有配置中；保留应用原有的模块/package 配置，
并将库路径替换为实际部署路径。模块需通过参数 API 初始化其参数。

```yaml
ibmw:
  extensions:
    items:
      - name: net_extension
        path: ./libibmw_net_transport.so
        options:
          thread_num: 2
          http_options:
            listen_ip: 127.0.0.1
            listen_port: 50080
      - name: parameter_extension
        path: ./libibmw_parameter_service.so
        options:
          service_name: car_a/parameters
  service:
    drivers:
      - type: http
    servers:
      - func_name: 'json:/car_a/parameters/.*'
        enable_drivers: [http]
```

对应调用需要加 `--service car_a/parameters`。省略服务端 `service_name` 和
客户端 `--service` 即使用默认名称。跨机使用 HTTP 时，服务端应监听可达的
网卡地址，客户端填写该设备的地址。其他后端按已有 service 部署方式配置。

服务端构建要求 `IBMW_BUILD_PARAMETER_EXTENSION=ON` 和相应后端开关；
部署 `libibmw_parameter_service.so`、`libibmw_parameter_types.so` 及后端依赖。
命令行 provider 为 `libibmw_param_cli.so`，已加入生成的 CLI 清单。
只部署客户端时可以关闭服务端参数扩展，参数 CLI 和协议库仍可构建。

## 值、错误和持久化

- 值始终是字符串：`0.5`、`false`、`[1, 2]` 不会被转换成数字、布尔或数组。
  `get` 默认显示 `String value is: ...`，`--hide-type` 只输出值和换行。
- `set` 可更新或创建已有模块的键，不会创建模块。模块名和键不能为空。
  空字符串是有效值，不表示删除。原有模块 C++ API 对已存在键的空值删除语义保留；
  旧 `Load` RPC 仍沿用该 API 的语义。本次未新增 `delete`、`dump` 或 `load` CLI。
- 新服务端会明确区分 `PARAMETER_NOT_FOUND` 与成功读取空值。
  旧服务端的缺失值/空值行为不同；需要上述语义时应同时升级服务端。
- 成功返回退出码 0；传输失败或业务错误返回非零，诊断写入 stderr。
  `--timeout` 默认 5 秒，`--discovery-timeout` 默认 2 秒，均为每次 RPC 的上限；
  `list` 所有模块会执行多次 RPC，初始化和清理也有开销。
- 每次 RPC 只发送一次，不自动重试。超时或 Ctrl+C 后，服务端仍可能已经执行 `set`。
- `set` 修改模块使用的运行时参数表，不修改 YAML、源码或启动文件。
  模块下次读取该键时能读到新值；若业务代码只在初始化时读取并缓存，
  需要由业务代码再次读取或自行处理更新。重启后的值由初始化逻辑决定。

## 回归测试

开启 `IBMW_BUILD_TESTS` 后，`ibmw_param_cli_test` 覆盖命令解析，
`ibmw_parameter_service_test` 覆盖共享参数表、错误和协议编解码。
`ibmw_param_<backend>_test` 使用独立服务进程验证实际网络调用、运行时读取、
模块隔离、字符串边界及重启后不保留修改；配置、日志和观察文件都生成在临时目录。
MQTT 测试另外需要 `mosquitto`。测试关闭 Runtime Graph，证明参数命令不依赖它。

```bash
ctest --test-dir <build-dir> -R '^(ibmw_param_.*|ibmw_parameter_service_test)$' --output-on-failure
```
