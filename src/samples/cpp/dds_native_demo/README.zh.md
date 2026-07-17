# DDS Native Demo

演示如何通过 IBMW 的 DDS transport 接口、用自定义 `HelloMsg` 类型做原生 DDS
publish/subscribe。publisher 与 subscriber 在同一个 app 模式进程内运行。

## 运行

```bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:${LD_LIBRARY_PATH}"

bash start_dds_native_demo.sh
# 或：./dds_native_app ./cfg/dds_native_cfg.yaml
```

跑出几条消息后按 Ctrl+C 退出。

## 期望输出

```
DDS Native Demo initialized.
Published HelloMsg: id=0, data="Hello DDS"
Received HelloMsg: id=0, data="Hello DDS"
Published HelloMsg: id=1, data="Hello DDS"
Received HelloMsg: id=1, data="Hello DDS"
...
```

## 配置

| 配置文件 | 说明 |
|----------|------|
| `dds_native_cfg.yaml` | DDS extension，domain_id=0 |

## 涉及概念

- 用 `IBMW_DDS_TYPE_NAME()` 宏注册自定义 DDS 类型（`HelloMsg`）
- `ibmw::transport::RegisterPublishType<T>()` —— DDS 类型注册
- `GenericDdsPubSubType<T>` —— 基于 FastCDR 的 TopicDataType 自动适配器
- 单进程 DDS loopback（publisher 与 subscriber 同属一个 DDS participant）

---

## ibmw-gen 方式

本 Demo 也可由 `ibmw-gen` 从
[`dds_native_demo.ibmw.yaml`](./dds_native_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 自动生成 | 你来写 |
|----------|--------|
| `CMakeLists.txt` —— 含 FastCDR 链接的构建系统 | `publisher_module.cc` —— 发布逻辑 |
| `protocol/hello_msg.h` —— C++ struct + 内联 CDR 序列化 | `subscriber_module.cc` —— 接收回调 |
| `cfg/dds_native_app.yaml` —— 含 DDS driver 的 App 模式配置 | Module `.h` 头文件 |
| `cfg/dds_native_pkg.yaml` —— Pkg 模式配置 | |
| `src/dds_native_app/main.cc`、`src/*_pkg/pkg_main.cc` | |

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py dds_native_demo.ibmw.yaml -o dds_native_demo_gen

# 构建
mkdir -p dds_native_demo_gen/build && cd dds_native_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行（App 模式）
./dds_native_app cfg/dds_native_app.yaml

# 运行（Pkg 模式）
ibmw run cfg/dds_native_pkg.yaml
```

### 关键 YAML 字段

```yaml
custom_dds_types:
  namespace: "ibmw::examples::dds_native_demo"
  messages:
    - name: HelloMsg
      constructor_args: true            # 生成带字段参数的构造函数
      fields:
        - { name: id, type: uint64, default: "0" }
        - { name: message, type: string }
        - { name: timestamp, type: int64, default: "0" }
```

ibmw-gen 会生成完整的 `HelloMsg` C++ struct，包含 `IBMW_DDS_TYPE_NAME` 宏、
FastCDR 的 `serialize` / `deserialize` 方法，以及 `GenericDdsPubSubType` 适配器
—— 既不必写 IDL，也不必手写 CDR 代码。Module 脚手架已经提供类型化的
`SenderAccess<HelloMsg>` / `ReceiverAccess<HelloMsg>` 包装。
