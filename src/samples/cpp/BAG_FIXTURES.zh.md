# 示例 bag fixture 清单

仓库中的录制 bag 是二进制文件，无法按文本评审。这里登记每个文件的 SHA-256、
内容和用途；`scripts/check_release_hygiene.py` 会检查工作树中的每个 `.mcap`、
`.mcap.lz4`、`.mcap.zst`、`.db3` 都在本表中且哈希一致。新增或重新录制 bag 时，
请同步更新本表。`.gitattributes` 把这些文件标为 binary，git 不会对它们做文本
diff 或合并。

## 来源

这些 bag 由 IBMW 自身录制，在以下提交中加入，提交中没有记录录制命令：

- `playback_demo`：提交 5e83df5（2026-08-14），`ibmw::protocols::common::Header`
  消息，topic `playback_demo_topic`，serialization type `cdr`。
- `bag_convert_demo`：提交 135ae29（2026-08-14），topic `/sequence_probe`。
- `info_smoke_test`、`convert_smoke_test`：提交 77a26be（2026-08-19）。topic
  `/ibmw_record_smoke/features/data` 与 `record_smoke_test/validate_record_smoke.sh`
  录制的数据 topic 相同。

内容可以用 `ibmw bag info <bag 目录>` 查看，下表中的条数和时长就是这样得到的。

## 重复文件

有两组文件字节完全相同：`playback_demo` 与 `info_smoke_test` 的 `mcap`、`sqlite3`，
以及 `bag_convert_demo` 与 `convert_smoke_test` 的 `ibmw_mcap`。每个示例目录在构建时
单独把自己的 `bags/` 复制到构建目录，示例之间互不依赖，所以保留副本；更新其中
一份时，请同时更新另一份和本表。

## 清单

| 文件 | SHA-256 | 内容 | 说明 |
| --- | --- | --- | --- |
| `src/samples/cpp/bag_convert_demo/bags/ibmw_mcap/data_0.mcap` | `b6416e73e3f24e1123c800f3afbac863aac37f31b55efc693df631b4876780fd` | `/sequence_probe`，5 条，0.40 s，无压缩 | 与 convert_smoke_test 的 ibmw_mcap 字节相同 |
| `src/samples/cpp/convert_smoke_test/bags/ibmw_mcap/data_0.mcap` | `b6416e73e3f24e1123c800f3afbac863aac37f31b55efc693df631b4876780fd` | `/sequence_probe`，5 条，0.40 s，无压缩 | 与 bag_convert_demo 的 ibmw_mcap 字节相同 |
| `src/samples/cpp/info_smoke_test/bags/empty_mcap/data_0.mcap` | `fc1f8e0967cd044d6a34665fb1b7cd2cd66e73118fa24d6530478b98200b2c61` | 无 topic，0 条 | 空 MCAP |
| `src/samples/cpp/info_smoke_test/bags/file_lz4_mcap/data_0.mcap.lz4` | `170d4880e92d2f37c5bda302ec5331824b3b2c55e7dbe9ea6bf17bbd346266e1` | `/ibmw_record_smoke/features/data`，13 条，1.20 s | 文件级 lz4 压缩 |
| `src/samples/cpp/info_smoke_test/bags/mcap/data_0.mcap` | `b247615b1e5d9f8e4c88707d594843c77f8c29f23c05b337988f52fba0a42dbc` | `playback_demo_topic`，5 条，4.00 s，无压缩 | 与 playback_demo 的 mcap 字节相同 |
| `src/samples/cpp/info_smoke_test/bags/message_zstd_mcap/data_0.mcap` | `c28484bb45ab15d2839c38c2c64b576d8c422ed333b0381926a317e360b12dd8` | `/ibmw_record_smoke/features/data`，13 条，1.20 s | 消息级 zstd 压缩 |
| `src/samples/cpp/info_smoke_test/bags/multi_file_mcap/data_0.mcap` | `28ff4be916a055153333094c49e01c73dcd945f3604296be623d366be70d65dd` | `/ibmw_record_smoke/features/data`，两个文件共 19 条，1.80 s | 多文件 bag 的第 1 个文件 |
| `src/samples/cpp/info_smoke_test/bags/multi_file_mcap/data_1.mcap` | `f41c9b77dc56387872449f5d15d2104e55ab362770457a348f107bb8e47f133d` | 同上 | 多文件 bag 的第 2 个文件 |
| `src/samples/cpp/info_smoke_test/bags/sqlite3/data_0.db3` | `6ab9baf99899603acaa7e72dc2b467b521104779c0c3a6f9593b6ffb15c3b9d1` | `playback_demo_topic`，5 条，4.00 s | 与 playback_demo 的 sqlite3 字节相同 |
| `src/samples/cpp/playback_demo/bags/mcap/data_0.mcap` | `b247615b1e5d9f8e4c88707d594843c77f8c29f23c05b337988f52fba0a42dbc` | `playback_demo_topic`，5 条，4.00 s，无压缩 | 与 info_smoke_test 的 mcap 字节相同 |
| `src/samples/cpp/playback_demo/bags/sqlite3/data_0.db3` | `6ab9baf99899603acaa7e72dc2b467b521104779c0c3a6f9593b6ffb15c3b9d1` | `playback_demo_topic`，5 条，4.00 s | 与 info_smoke_test 的 sqlite3 字节相同 |
