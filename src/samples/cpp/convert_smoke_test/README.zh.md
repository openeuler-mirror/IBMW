# `bag convert` 冒烟测试

`convert_smoke_test` 用于验证 IBMW 提供的 `ibmw bag convert` 命令。测试使用本目录
维护的输入 bag 和配置文件，直接检查不同 bag 格式和存储类型之间的转换结果。

转换矩阵如下：

```text
IBMW MCAP    -> IBMW SQLite3
IBMW SQLite3 -> IBMW MCAP

IBMW MCAP    -> rosbag2 MCAP
IBMW MCAP    -> rosbag2 SQLite3

rosbag2 MCAP    -> IBMW SQLite3
rosbag2 SQLite3 -> IBMW MCAP
```

## 构建

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake --preset ros2 -B build-convert-smoke
cmake --build build-convert-smoke \
  --target ibmw_samples_cpp_convert_smoke_test_build_all \
  -j$(nproc)
```

构建目标会把输入 bag、storage 配置和验证脚本复制到 `build-convert-smoke`。

## 手动运行

```bash
cd <IBMW_ROOT>/build-convert-smoke
./validate_convert_smoke.sh
```

通过时输出：

```text
PASS: bag convert smoke conversion and CLI matrix
```

每次运行都会保留日志和所有转换结果，并在最后打印 artifact 目录。例如：

```text
convert smoke artifacts: /tmp/ibmw-convert-smoke.XXXXXX
```

失败时会打印失败用例和对应的日志路径，并返回非 0。

## 测试覆盖

脚本验证：

- `--help`、`-h`；
- 位置参数、`-i/--input`、`-o/--output` 以及 `--option=value` 形式；
- IBMW MCAP/SQLite3 互转；
- IBMW 到 rosbag2 MCAP/SQLite3；
- rosbag2 MCAP/SQLite3 到 IBMW；
- `--input-format`、`--output-format`、`--input-storage` 和 `--storage`；
- `--max-bag-size`，检查输出被按 size 拆分；
- `--max-bag-duration`，检查输出被按 duration 拆分；
- MCAP message/zstd 和 file/lz4 压缩；
- compression queue/thread 参数；
- MCAP/SQLite3 writer 和 reader storage 配置文件；
- MCAP `small` storage preset；
- 缺少参数、路径不存在、输入输出相同、格式/storage 不匹配、非法压缩组合、
  零 compression queue、未知格式和不存在的配置文件。

每条成功转换都会重新打开输出并检查：

- IBMW 输出使用 `ibmw bag info`；
- rosbag2 输出使用 `ros2 bag info`；
- 消息数必须仍为 5；
- `/sequence_probe` topic 必须存在；
- 类型必须是 `std_msgs/msg/String`；
- IBMW serialization 必须为 `ros2`，rosbag2 serialization format 必须为 `cdr`；
- 输出必须能完整读取，IBMW info 必须报告 `Complete playback: yes`；
- 压缩和拆分用例还必须检查 metadata、文件后缀或 `split_reason` 的具体结果。

## 纳入总冒烟测试

纳入总冒烟流程时，先用 `stage_smoke.sh` 收集运行所需的二进制、库、fixture 和
脚本，再通过统一入口执行测试：

```bash
cd <IBMW_ROOT>
BUILD="$PWD/build-convert-smoke" \
STAGE=/tmp/ibmw-convert-smoke-stage \
./scripts/stage_smoke.sh

cd /tmp/ibmw-convert-smoke-stage
./run_all_tests.sh --ext-only
```

统一入口中的测试名称为 `bag_convert_cli_and_conversion_matrix`，最长运行 180 秒。
`--quick` 会跳过该测试。
