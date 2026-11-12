<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# IBMW 日志命令

`ibmw log list/set` 通过已有的集中式 `LogControlService` 查看及修改 logger 的
运行时级别，复用 `service call` 的后端连接、就绪等待和超时流程。
`list` 列出所选服务对应运行时已登记的 logger，不扫描整个通信域，
也不读取日志文件或订阅日志正文。Module logger 与 `core` 都来自原有的
`LoggingSubsystem`，不增加 Runtime Agent 登记项。

## 使用

每次命令必须且只能选择一个已部署的 RPC 后端，本机与跨机使用相同方式：

```bash
ibmw log list --http 127.0.0.1:50080
ibmw log list CarModule --http 127.0.0.1:50080
ibmw log set CarModule DEBUG --http 127.0.0.1:50080
ibmw log set core WARN --http 127.0.0.1:50080

ibmw log list --dds --domain 10
ibmw log set CarModule debug --dds --domain 10
ibmw log list --grpc 127.0.0.1:50051
ibmw log list --zenoh --zenoh-domain car
ibmw log list --mqtt tcp://127.0.0.1:1883 --mqtt-server-id car
```

`list [module]` 每行输出 `名称 级别`，按名称排序，级别统一大写，例如：

```text
CarModule INFO
OtherModule WARN
core INFO
```

`set <module> <level>` 只修改已存在的 logger。级别为
`TRACE`、`DEBUG`、`INFO`、`WARN`、`ERROR`、`FATAL`、`OFF`，不区分大小写。
空名称、非法级别及未知 logger 都报错，不会创建 logger。
支持 `core`，以及运行时登记的其他 logger 名称；名称须完全一致。

命令支持 DDS、HTTP、gRPC、MQTT 和 Zenoh。DDS 使用 CDR，其他后端默认 JSON；
实际可用性由构建和部署决定。协议类型已内置，无需 `--type-support`。
没有 `--ros2`、`--runtime` 或 `--registry`。其他连接选项参见
[`service call`](user-guide.zh.md#调用-service)。

## 服务选择和部署

保留现有方法及 JSON 字段：

| 命令 | 方法 | 请求 |
| --- | --- | --- |
| `list` | `GetModuleLogLevel` | `{"module_names":[]}` |
| `list CarModule` | `GetModuleLogLevel` | `{"module_names":["CarModule"]}` |
| `set CarModule DEBUG` | `SetModuleLogLevel` | `{"module_log_level_map":{"CarModule":"DEBUG"}}` |

默认服务基础名称为 `ibmw.protocols.log_control_extension.LogControlService`。
完整方法名例如 `json:/ibmw.protocols.log_control_extension.LogControlService/GetModuleLogLevel`。
服务端可配置 `service_name`，客户端对应传 `--service`；此选项填基础名称，
不带 `json:` 前缀或方法后缀。

以下内容合并到应用现有配置，保留原有模块/package 配置，替换实际库路径：

```yaml
ibmw:
  logging:
    core_level: INFO
    default_module_level: INFO
    backends:
      - type: console
  extensions:
    items:
      - name: net_extension
        path: ./libibmw_net_transport.so
        options:
          thread_num: 2
          http_options:
            listen_ip: 127.0.0.1
            listen_port: 50080
      - name: log_control_extension
        path: ./libibmw_log_control.so
        options:
          service_name: car_a/logs
  service:
    drivers:
      - type: http
    servers:
      - func_name: 'json:/car_a/logs/.*'
        enable_drivers: [http]
```

```bash
ibmw log list --http 127.0.0.1:50080 --service car_a/logs
```

省略服务端 `service_name` 和客户端 `--service` 即使用默认名称。
同一通信域内的多实例应使用不同的基础名称，如 `car_a/logs`、`car_b/logs`；
模块名称只选择服务内部的 logger，不推导其他服务端点。
HTTP/gRPC 可用不同地址和端口，MQTT 还可用 `--mqtt-server-id` 区分实例。
多个可达实例暴露相同服务名时，不保证选中哪一个。
跨机 HTTP 部署需要监听可达网卡地址，并在客户端填写服务端地址。

服务端需启用 `IBMW_BUILD_LOG_CONTROL_EXTENSION=ON` 及所选后端开关，部署
`libibmw_log_control.so`、`libibmw_log_control_types.so` 及后端依赖。
`libibmw_log_cli.so` 已加入自动生成的 CLI 清单。仅部署客户端时可以关闭
日志服务扩展，CLI 和协议库仍可构建。

## 生效范围和错误

- `set` 更新模块正在使用的同一个 logger 的原子级别值，对后续日志过滤生效。
  修改一个 logger 不影响其他 logger；修改 `core` 也不会批量修改 Module。
- 启用更低阈值不会绕过已有输出后端的其他过滤条件，也不会重放已过滤的日志。
  没有配置日志输出后端时，修改级别仍成功，但不会产生 console/file 输出。
- 不修改 YAML、源码、启动文件或历史日志文件。重启后按启动配置重新初始化。
- 保留原有 JSON 字段和服务返回的级别拼写（例如 `Info`）；CLI 统一显示为 `INFO`。
  新增 DDS/CDR 适配。旧 JSON 服务端可能对非法输入返回成功，需要升级服务端才能
  获得严格校验保证。
- 服务业务码：0 成功；1 `INVALID_MODULE_NAME`；2 `INVALID_LOG_LEVEL`；
  3 `EMPTY_SETTINGS`。直接调用批量 set 时，先校验全部条目，再修改；
  不承诺多个 logger 对并发读者表现为同一事务。
- 成功退出码为 0；传输失败和业务错误均非零，诊断写入 stderr。
  `set` 成功输出 `Set log level successful`。`--timeout` 默认 5 秒，
  `--discovery-timeout` 默认 2 秒，初始化与清理另有开销。
  不自动重发请求；超时或中断不代表服务端一定没有执行修改。

## 回归测试

`ibmw_log_control_service_test` 检查实际 logger、模块隔离、批量校验、错误回调和
JSON/CDR 编解码；`ibmw_log_cli_test` 检查命令参数。
`ibmw_log_<backend>_test` 启动独立服务进程，经真实后端调用并检查实际日志过滤、
中文及长名称、服务不可用和重启行为。测试关闭 Runtime Graph。
MQTT 用例需要 `mosquitto`。测试配置、日志、探针文件均位于临时目录。

```bash
ctest --test-dir <build-dir> -R '^ibmw_log_' --output-on-failure
```
