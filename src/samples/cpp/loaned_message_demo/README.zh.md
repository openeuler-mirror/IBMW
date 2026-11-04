# LoanedMessage Demo

IBMW LoanedMessage API 与零拷贝消息发布的端到端验证 Demo。

## 特性

- 在 ROS2 兼容 DDS 通道上用 LoanedMessage 发布 `geometry_msgs::msg::Point`。
  ROS2 消息类型按设计注册为 non-plain，因此 AUTO 走堆内存 fallback（由
  `DataWriter::write` 拷贝发送）。真正的 `loan_sample` 零拷贝通道（原生 plain
  DDS 类型）见 [transport benchmark](../transport_benchmark/README.md) 中的 `dds_loaned/`。
- Non-Plain 类型（`std_msgs::msg::String`）的自动 fallback 验证
- 三种配置模式测试（AUTO / STRICT / COMPAT）
- 零拷贝统计输出
- 支持 ROS2 互操作

## 构建

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake -B build \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_EXAMPLES=ON

# 单线程构建以避免日志丢失
cmake --build build --target ibmw_dds_extension -- -j1
cmake --build build --target loaned_message_app -- -j1
cmake --build build --target config_test_app -- -j1
cmake --build build --target plain_pub_only_app -- -j1
cmake --build build --target nonplain_pub_only_app -- -j1
cmake --build build --target sub_only_app -- -j1
```

## 运行

```bash
cd build
source /opt/ros/humble/setup.bash

# 完整端到端测试（按 Ctrl+C 退出）
./loaned_message_app ../src/samples/cpp/loaned_message_demo/config/cfg/loaned_message_cfg.yaml

# 配置模式测试（自动跑完）
./config_test_app ../src/samples/cpp/loaned_message_demo/config/cfg/config_test_cfg.yaml

# 单项测试
./plain_pub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/plain_pub_only_cfg.yaml
./nonplain_pub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/nonplain_pub_only_cfg.yaml
./sub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/sub_only_cfg.yaml
```

## 应用程序

| App | 说明 |
|-----|------|
| loaned_message_app | 完整测试：Plain + NonPlain 发布 + 订阅 |
| config_test_app | 6 种配置模式测试场景 |
| plain_pub_only_app | Point 发布（ROS2 类型，AUTO 堆内存 fallback） |
| nonplain_pub_only_app | Non-Plain 类型 fallback 验证 |
| sub_only_app | 订阅端 |

## 配置模式

| 模式 | Plain 类型 | Non-Plain 类型 |
|------|------------|----------------|
| AUTO | 零拷贝（若已开启 SHM） | 自动 fallback 到堆内存 |
| STRICT | 零拷贝（失败时返回 invalid） | 返回 invalid 对象 |
| COMPAT | 强制走堆内存 | 堆内存 |

## 配置文件

| 文件 | 说明 |
|------|------|
| loaned_message_cfg.yaml | 完整测试（AUTO 模式） |
| strict_mode_cfg.yaml | STRICT 模式 |
| compat_mode_cfg.yaml | COMPAT 模式 |
| config_test_cfg.yaml | ConfigTestModule 专用 |
| plain_pub_only_cfg.yaml | Plain 发布 |
| nonplain_pub_only_cfg.yaml | Non-Plain 发布 |
| sub_only_cfg.yaml | 订阅 |

## 期望输出

Point（ROS2 类型，AUTO 堆内存 fallback）：
```
[SEND] Point[0] x=1.00, y=2.00, z=3.00, is_zero_copy=false, is_plain=false, ptr=0x7f...
[RECV] Point[1] x=1.10, y=2.20, z=3.30, valid=YES, ptr=0x7f...
```

Non-Plain 类型（fallback）：
```
[SEND] String[0] borrow ok: is_zero_copy=false, is_plain=false, is_valid=true
[RECV] String[1] data="Hello LoanedMessage #0", valid=YES
```

配置模式测试：
```
Config Test Summary: 6/6 passed
All tests passed!
```

## ROS2 互操作

```bash
# 终端 1：IBMW 发布
./plain_pub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/plain_pub_only_cfg.yaml

# 终端 2：ROS2 订阅
ros2 topic echo /rt/point_topic geometry_msgs/msg/Point
```

## 关于 ibmw-gen

本 Demo 使用 **DDS extension 内部 API**（`DdsTransportDriver`、
`Ros2StylePublisher`、`BorrowLoanedMessage`、`ZeroCopyConfig`）实现零拷贝发布。
这是 `ibmw-gen` 代码生成器不覆盖的专用底层 API。要了解 ibmw-gen 在标准 pub/sub
与 RPC 项目中的用法，参见 [ibmw-gen README](../../../tools/ibmw_gen/README.md)。

## 排错

参见 config/docs/TROUBLESHOOTING.md。
