# Bag Convert 示例

本示例使用一个预置的 IBMW MCAP bag，验证三类转换：

1. IBMW 内部的 MCAP 与 SQLite3 互转；
2. IBMW 转换为 rosbag2 的 MCAP 和 SQLite3；
3. rosbag2 的 MCAP 和 SQLite3 转换回 IBMW。

每次转换后使用 `ibmw bag info` 或 `ros2 bag info` 检查结果。本示例只验证
convert 和 info，不启动 publisher、subscriber 或 Transport Runtime。

预置 bag 包含 5 条 `/sequence_probe` 消息，类型为 `std_msgs/msg/String`。IBMW
元数据中的类型名称是 `ros2:std_msgs/msg/String`，消息负载使用 ROS2-compatible
CDR，因此转换后的 rosbag2 可以直接识别该标准消息类型。

## 核心文件

- [CMakeLists.txt](./CMakeLists.txt)
- [run_bag_convert_demo.sh](./config/linux/bin/run_bag_convert_demo.sh)
- [预置 IBMW MCAP](./bags/ibmw_mcap/metadata.yaml)

## 转换关系

```text
预置 IBMW MCAP
  ├─ IBMW SQLite3 ──> IBMW MCAP
  ├─ rosbag2 MCAP ──> IBMW SQLite3
  └─ rosbag2 SQLite3 ──> IBMW MCAP
```

MCAP 和 SQLite3 是存储格式；IBMW 和 rosbag2 是 bag 生态及元数据格式。两组概念
不能混为一谈，例如 IBMW MCAP 和 rosbag2 MCAP 虽然都使用 MCAP 文件，但 Header、
Schema 和 `metadata.yaml` 的约定不同。

## 构建

使用新的构建目录，不依赖其他 demo 的构建产物：

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake --preset minimal -B build-bag-convert-demo \
  -DCMAKE_BUILD_TYPE=Release \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON

cmake --build build-bag-convert-demo \
  --target ibmw_samples_cpp_bag_convert_demo_build_all \
  -j$(nproc)
```

聚合目标会重新构建 `ibmw` 和 record/playback extension，并把脚本及预置输入复制到
`build-bag-convert-demo`。本示例不依赖 `build`、`build-record-demo` 或
`build-playback-demo` 中的文件。

## 一键运行

```bash
cd <IBMW_ROOT>/build-bag-convert-demo
source /opt/ros/humble/setup.bash
bash ./run_bag_convert_demo.sh
```

脚本依次执行三类转换并显示全部 info。默认输出目录是：

```text
bag_convert_demo_output/
├── 01_ibmw_sqlite3/
├── 02_ibmw_mcap_roundtrip/
├── 03_ros2_mcap/
├── 04_ros2_sqlite3/
├── 05_ros2_mcap_to_ibmw_sqlite3/
└── 06_ros2_sqlite3_to_ibmw_mcap/
```

脚本不会覆盖已有输出。再次运行时指定一个尚不存在的目录：

```bash
bash ./run_bag_convert_demo.sh ./bag_convert_demo_output_2
```

## 分步运行

以下命令与一键脚本一致，适合逐步观察转换过程。
这些命令使用固定的 `./bag_convert_demo_output` 输出根目录，因此执行前该目录必须
不存在。若已经运行过一键脚本或下面的分步命令，请把本节命令中的
`bag_convert_demo_output` 统一替换为一个尚不存在的新目录名；转换命令不会覆盖已有
bag。

### 1. IBMW 内部转换

```bash
./ibmw bag convert ./bag_convert_demo_input/ibmw_mcap \
  -o ./bag_convert_demo_output/01_ibmw_sqlite3 \
  --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage sqlite3

./ibmw bag convert ./bag_convert_demo_output/01_ibmw_sqlite3 \
  -o ./bag_convert_demo_output/02_ibmw_mcap_roundtrip \
  --input-format ibmw --output-format ibmw \
  --input-storage sqlite3 --storage mcap

./ibmw bag info ./bag_convert_demo_input/ibmw_mcap
./ibmw bag info ./bag_convert_demo_output/01_ibmw_sqlite3
./ibmw bag info ./bag_convert_demo_output/02_ibmw_mcap_roundtrip
```

### 2. IBMW 转 rosbag2

```bash
./ibmw bag convert ./bag_convert_demo_input/ibmw_mcap \
  -o ./bag_convert_demo_output/03_ros2_mcap \
  --input-format ibmw --output-format rosbag2 \
  --input-storage mcap --storage mcap

./ibmw bag convert ./bag_convert_demo_input/ibmw_mcap \
  -o ./bag_convert_demo_output/04_ros2_sqlite3 \
  --input-format ibmw --output-format rosbag2 \
  --input-storage mcap --storage sqlite3

ros2 bag info ./bag_convert_demo_output/03_ros2_mcap
ros2 bag info ./bag_convert_demo_output/04_ros2_sqlite3
```

rosbag2 输出应显示 `Topic: /sequence_probe`、`Type: std_msgs/msg/String` 和
`Count: 5`。

### 3. rosbag2 转 IBMW

```bash
./ibmw bag convert ./bag_convert_demo_output/03_ros2_mcap \
  -o ./bag_convert_demo_output/05_ros2_mcap_to_ibmw_sqlite3 \
  --input-format rosbag2 --output-format ibmw \
  --input-storage mcap --storage sqlite3

./ibmw bag convert ./bag_convert_demo_output/04_ros2_sqlite3 \
  -o ./bag_convert_demo_output/06_ros2_sqlite3_to_ibmw_mcap \
  --input-format rosbag2 --output-format ibmw \
  --input-storage sqlite3 --storage mcap

./ibmw bag info \
  ./bag_convert_demo_output/05_ros2_mcap_to_ibmw_sqlite3
./ibmw bag info \
  ./bag_convert_demo_output/06_ros2_sqlite3_to_ibmw_mcap
```

## 验证标准

所有结果都应满足：

- topic 为 `/sequence_probe`；
- type 对应 `std_msgs/msg/String`；
- message count 为 5；
- serialization 为 ROS2-compatible CDR；
- IBMW `bag info` 显示 `Closed cleanly: yes` 和 `Complete playback: yes`。

转换过程不反序列化并重新编码消息对象，而是保留原始 CDR payload、时间戳、topic、
Schema 和可映射的 QoS 信息，并按照目标 bag 生态重新写入存储及元数据。
