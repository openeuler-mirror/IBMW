# PR #5 必修问题修复与验证报告

本报告记录 `IBMW_PR5_REVIEW.md` 中必须修复项的本地修复、长期回归测试和实际验证结果。所有修改均在 `master` 工作树完成，未推送远端；本轮修复通过定向交互式 `rebase`/`fixup` 归入对应的修复链，未留下新的普通功能提交。

## 修复范围

| 问题 | 修复要点 | 长期回归覆盖 |
| --- | --- | --- |
| Publish tap 自注销/回调内 shutdown 死锁 | 调用上下文感知的停用等待；当前回调只等待其他线程，退出回调后再完成回收 | `PublishTapTest.SelfUnregisterDoesNotDeadlockAndPreventsReentry`、`ShutdownFromCallbackDoesNotDeadlockOrSendAfterShutdown`、外部注销等待测试 |
| TypeSupport reload 后悬空 span | 在候选 DSO 中完成符号和 descriptor 校验后再交换；失败不卸载当前有效包 | `TypeSupportPkgLoader.FailedReloadKeepsPreviouslyValidatedPackage`、`InvalidV1ReloadKeepsPreviouslyValidatedPackage` |
| 播放/录制停止与关闭阻塞 | stop 同时唤醒读写队列；关闭投递有界等待并传播 executor/close 异常 | `BagPlayerStopTest.*`、`RecordActionExecutorFixture.*`、真实 `PlaybackActionLockRegression.ConcurrentStopReadCloseCompletes` |
| Bag 路径越界 | 仅接受相对路径，拒绝 `..`、绝对路径和 canonical 后逃逸/符号链接 | `BagPathSafetyTest.RejectsAbsoluteParentAndSymlinkEscapes` |
| 损坏 SQLite、ID/sequence 边界 | 检查 prepare/step/必需表、引用和重复 ID；跨格式 uint32 边界显式拒绝 | `SqliteBagReaderIntegrityTest.*`、MCAP/SQLite 内容回环 |
| ROS2 嵌套 schema 不一致 | introspection 与 rosbag2 使用一致的 `pkg/msg/Type` ros2msg 文本 | `Ros2SchemaSupport.PreservesMsgNamespaceForNestedCrossPackageArrays` |
| SchemaSupport ABI 演进 | 增加 `abi_version`、`struct_size`、reserved 与 loader 截断校验 | C/C++ 布局测试、截断 descriptor/package DSO fixture |
| Runtime discovery IPC 边界 | V1 ID/路径 containment、owner-only 权限、损坏/超大注册清理、现有 UID/超时/陈旧回收策略 | `RuntimeProtocolTest.*` |
| 生成包 transport 解耦 | 生成源码只引用 framework-owned public TypeSupport 头/target；禁止 extension-internal include/link | 生成器 5 项 unittest，含临时目录真实生成、configure、编译/link、幂等重生成 |
| Publish tap 热路径证据 | 复用 `transport_benchmark` 的真实 dispatcher 和 `LatencyStats`，提供无 tap、空闲 tap、owned-copy tap 三模式重复 p99/分配计数 | `ibmw_publish_tap_benchmark`（本机证据，不设跨机器固定阈值） |

四组 bag smoke（record/info/convert/play）均已注册为 CTest fixture + validator，设置 timeout、`RESOURCE_LOCK` 和 fixture 依赖；validator 采用严格退出状态，超时不会被视为成功。record/convert 使用独立 payload reader 验证字段、顺序、时间戳和跨格式内容。

## 实际验证

### 专项单元/集成测试

在 `build-rp-test` 构建并运行：

```text
ibmw_core_engine_test: PublishTapTest.* + TypeSupportPkgLoader.*  9/9 passed
ibmw_extensions_record_playback_ext_test: stop/close/runtime/path/SQLite/回环 20/20 passed
ibmw_api_type_support_test: SchemaSupportV1Abi.GoldenLayout passed
```

### 生成器测试

```text
python3 -m unittest -v src/tools/ibmw_gen/tests/test_type_support_pkg_emit.py
Ran 5 tests ... OK
```

测试在临时目录中执行完整生成、CMake configure、编译/link 和第二次 byte-equal regeneration；临时产物不写入源码树。

### 标准 DDS/ROS2 构建与 smoke

`build-smoke-runtime-graph` 使用 DDS + ROS2 + record/playback + examples 配置完成构建。最终全量 CTest 发现四组 smoke 的 fixture/validator，并实际运行：

```text
20/20 tests passed (record 59.90 s, play 42.92 s)
```

同一构建中的 DDS、ROS2、record/playback 和嵌套 ROS2 schema 测试均通过。

### Publish tap benchmark

三模式各重复 3 次、每次 200 warmup + 2000 measured sends：

```text
no_tap:         callbacks=0,    allocations≈6/repeat
idle_tap:       callbacks=2200, allocations≈2006/repeat
owned_copy_tap: callbacks=2200, allocations≈2006/repeat, queue_overwrites=1944
```

同时输出 p50/p90/p99/p99.9/max；数值是当前主机的重复证据，不作为跨机器固定性能门槛。

## 未覆盖/环境限制

完整 DDS/ROS2 send/receive smoke 依赖本机安装的 middleware 和运行时资源；本次标准依赖构建与四组 bag smoke 已通过。所有临时 bag、构建目录和 benchmark CSV 均位于构建树或 `/tmp`，没有把测试产物加入源码仓库。
