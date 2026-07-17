# Pub/Sub Demo

演示 IBMW transport（publish/subscribe），含 local 与 DDS 后端。
使用 IDL 生成的 `ExampleEventMsg` 类型并用 FastCDR 序列化。

## 运行

```bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:${LD_LIBRARY_PATH}"

# 单进程，local 后端（pkg 模式）
bash start_pubsub_local.sh

# 单进程，DDS 后端（app 模式）
bash start_pubsub_dds.sh

# 双进程，DDS 后端（pkg 模式）
# 终端 1：
bash start_publisher_dds.sh
# 终端 2：
bash start_subscriber_dds.sh
```

## 预期输出

Publisher：
```
Published: msg='HelloWorld', num=0
Published: msg='HelloWorld', num=1
Published: msg='HelloWorld', num=2
...
```

Subscriber：
```
Received: msg='HelloWorld', num=0
Received: msg='HelloWorld', num=1
Received: msg='HelloWorld', num=2
...
```

## 配置

| 配置文件 | 模式 | 后端 | 说明 |
|--------|------|---------|-------------|
| `pubsub_single_process_cfg.yaml` | pkg | local | 两个 module 在同一进程 |
| `pubsub_dds_cfg.yaml` | app | DDS | 两个 module 通过 DDS 在同一进程 |
| `publisher_dds_cfg.yaml` | pkg | DDS | 仅 publisher（独立进程） |
| `subscriber_dds_cfg.yaml` | pkg | DDS | 仅 subscriber（独立进程） |
| `publisher_local_cfg.yaml` | pkg | local | 仅 publisher（local 后端） |
| `subscriber_local_cfg.yaml` | pkg | local | 仅 subscriber（local 后端） |

## 涉及概念

- `ibmw::transport::RegisterPublishType<T>(publisher)` —— 注册 DDS 发布类型
- `ibmw::transport::Subscribe<T>(subscriber, callback)` —— 用类型化回调订阅
- `ibmw::transport::Send(publisher, msg)` —— 发布一条消息
- 通过 YAML `enable_backends` 路由 local 与 DDS 后端
- 跨进程 DDS 通信（约 74us 延迟）

## IDL 类型

`ExampleEventMsg` 类型定义在 `src/schema/dds_idl/example/ExampleEvent.idl`：

```idl
module pubsub_demo {
  struct ExampleEventMsg {
    string msg;
    long num;
  };
};
```

---

## ibmw-gen 方式

本 demo 也可以通过 `ibmw-gen` 从
[`transport_demo.ibmw.yaml`](./transport_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 生成（自动） | 你自己写 |
|-------------------|-----------|
| `CMakeLists.txt` —— 带 DDS 链接的构建系统 | `publisher_module.cc` —— publish 逻辑（timer、消息构造） |
| `cfg/pubsub_app.yaml` —— App 模式配置（含 DDS driver） | `subscriber_module.cc` —— 接收回调逻辑 |
| `cfg/*_pkg.yaml` —— Pkg 模式配置 | `publisher_module.h`、`subscriber_module.h` —— 头文件 |
| `src/pubsub_app/main.cc` —— App 入口 | |
| `src/*_pkg/pkg_main.cc` —— Pkg 加载器 | |
| DDS 类型桥接头文件（引用外部 IDL target） | |

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py transport_demo.ibmw.yaml -o transport_demo_gen

# 构建
mkdir -p transport_demo_gen/build && cd transport_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行（App 模式）
./pubsub_app cfg/pubsub_app.yaml

# 运行（Pkg 模式）
ibmw run cfg/pubsub_pkg.yaml
```

### 关键 YAML 特性

```yaml
dds_types:
  namespace: ibmw::protocols::example
  groups:
    - name: ExampleEvent
      idl_gencode_target: ibmw::schema::example_idl_gencode  # 外部 IDL target
      messages:
        - name: ExampleEventMsg
          fields:
            - { name: msg, type: string }
            - { name: num, type: int32 }

modules:
  - name: PublisherModule
    type: publisher
    transport: dds              # DDS transport 后端
    topic: hello_topic
    message_type: ExampleEventMsg
```

ibmw-gen 为 publisher 生成 `SenderAccess<ExampleEventMsg>`，为 subscriber
生成 `ReceiverAccess<ExampleEventMsg>`，并附带类型化的 publish/subscribe
封装。DDS 类型桥引用外部已构建好的 IDL gencode target。
