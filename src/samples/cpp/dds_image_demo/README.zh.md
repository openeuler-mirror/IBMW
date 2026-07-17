# DDS Image Demo

演示 DDS 在大 payload 下的高吞吐通信：640x480 RGB8 图像 @10Hz（约 1MB / 帧）。
用于检验 DDS 序列化性能与 FastCDR 对大块二进制数据的处理。

## 运行

```bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:${LD_LIBRARY_PATH}"

bash start_dds_image_demo.sh
# 或：./dds_image_app ./cfg/dds_image_cfg.yaml
```

跑出几帧后按 Ctrl+C 退出。

## 期望输出

```
DDS Image Demo initialized.
Published image: 640x480, size=921600 bytes, frame=0
Received image: 640x480, size=921600 bytes, frame=0, latency=0.8ms
Published image: 640x480, size=921600 bytes, frame=1
Received image: 640x480, size=921600 bytes, frame=1, latency=0.9ms
...
```

## 配置

| 配置文件 | 说明 |
|----------|------|
| `dds_image_cfg.yaml` | DDS extension，10Hz 图像 pub/sub |

## 涉及概念

- 用 FastCDR 做大消息序列化
- 高吞吐 DDS 通信（持续约 10MB/s）
- 约 1MB payload 下的亚毫秒延迟
- 携带二进制像素数据的 `ImageMsg` 类型

---

## ibmw-gen 方式

本 Demo 也可由 `ibmw-gen` 从
[`dds_image_demo.ibmw.yaml`](./dds_image_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 自动生成 | 你来写 |
|----------|--------|
| `CMakeLists.txt` —— 含 FastCDR 链接的构建系统 | `image_publisher.cc` —— 图像生成 + 发布 |
| `protocol/image_msg.h` —— ImageMsg struct + ImageEncoding enum + 内联 CDR | `image_subscriber.cc` —— 接收 + 延迟计算 |
| `cfg/dds_image_app.yaml` —— App 模式配置 | Module `.h` 头文件 |
| `cfg/dds_image_app.yaml` —— App 模式配置 | |
| `cfg/dds_image_pkg.yaml` —— Pkg 模式配置 | |
| `src/dds_image_app/main.cc`、`src/*_pkg/pkg_main.cc` | |

### 关键 YAML 字段

```yaml
custom_dds_types:
  namespace: "ibmw::examples::dds_image_demo"
  constants:
    - { name: kImageBufferSize, type: uint64, value: 1048576 }
  enums:
    - name: ImageEncoding
      underlying_type: uint32
      values:
        - { name: RGB8, value: 0 }
        - { name: GRAY8, value: 2 }
  messages:
    - name: ImageMsg
      fields:
        - { name: encoding, type: ImageEncoding, default: "ImageEncoding::RGB8" }
        - { name: data, type: "array<uint8, kImageBufferSize>" }  # 1MB 像素缓冲
```

这演示了 ibmw-gen 的 `custom_dds_types`：含 constant、enum、定长 array 字段。
生成的 CDR 序列化通过 FastCDR 的批量拷贝高效处理 1MB 大小的
`array<uint8, kImageBufferSize>` 字段。
