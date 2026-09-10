# `bag info` 冒烟测试

`info_smoke_test` 用于验证 `ibmw bag info` 能否正确打开并报告已有的 bag，覆盖
命令行解析、存储类型选择和输出内容。测试直接读取独立 fixture，无需启动 Runtime
或发布消息。

测试目录自带独立 fixture，包括普通 MCAP、SQLite3、多文件 MCAP、message/zstd、
file/lz4 和空 MCAP。fixture 在构建时复制到构建目录，测试运行时读取构建目录中的
副本。

## 构建

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake --preset ros2 -B build-info-smoke
cmake --build build-info-smoke \
  --target ibmw_samples_cpp_info_smoke_test_build_all \
  -j$(nproc)
```

## 手动运行

```bash
cd <IBMW_ROOT>/build-info-smoke
./validate_info_smoke.sh
```

通过时输出：

```text
PASS: bag info smoke CLI and metadata matrix
```

脚本会保留本次运行日志，并打印 artifact 目录。也可以显式指定构建目录和 fixture
目录：

```bash
BUILD_DIR=<IBMW_ROOT>/build-info-smoke \
BAGS_DIR=<IBMW_ROOT>/build-info-smoke/info_smoke_bags \
<IBMW_ROOT>/build-info-smoke/validate_info_smoke.sh
```

## 覆盖内容

测试会依次验证：

- `bag info --help` 和 `bag info -h` 能显示 info 用法和 `--storage` 选项；
- 默认读取 MCAP，并报告 storage、文件、消息、topic、serialization、schema 和
  完整性字段；
- `-s mcap`、`--storage mcap` 和 `--storage=sqlite3` 显式选择存储插件；
- 默认读取 SQLite3；
- 多文件 bag 的文件数、逐文件信息和 `split_reason`；
- message/zstd 与 file/lz4 的 compression 输出和完整读取；
- 空 bag 报告 0 条消息且仍然完整可读；
- 没有传入 bag 路径时失败；
- `--storage` 缺少值或指定未知 storage 时失败；
- storage 类型不匹配时失败；
- 不存在的路径、未知选项、同时传入多个 bag 路径时失败；
- metadata 存在但数据文件缺失时失败。

正常 bag 的通过条件包括：

- `Messages: 5`，topic 消息数也是 5；
- topic、消息类型和 `cdr` serialization 正确；
- 文件信息中列出对应的 `.mcap` 或 `.db3` 文件；
- `Closed cleanly: yes`、`Dropped messages: 0`、`Duplicate copies: 0`；
- schema 显示 `valid=yes`；
- `Complete playback: yes`。

这里的 `Complete playback: yes` 表示 info 能完整读取 bag 的文件、channel、schema
和 payload。

## 纳入总冒烟测试

总冒烟流程会先把 info 测试产物复制到 stage，再从 stage 执行同一个验证脚本：

```bash
cd <IBMW_ROOT>
BUILD="$PWD/build-info-smoke" \
STAGE=/tmp/ibmw-info-smoke-stage \
./scripts/stage_smoke.sh

cd /tmp/ibmw-info-smoke-stage
./run_all_tests.sh --ext-only
```

统一入口中的测试名称为 `bag_info_cli_and_metadata_matrix`，最长运行 120 秒。
`--quick` 会跳过该测试。
