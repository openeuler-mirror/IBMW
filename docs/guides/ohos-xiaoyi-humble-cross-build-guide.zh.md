# OHOS ARM64 + ROS2 Humble 交叉构建指南

本文说明如何使用 OHOS Native SDK 与目标端 ROS2 Humble 前缀构建 IBMW。
该流程证明目标产物可编译并通过架构审计；除非另有设备运行记录，否则不代表
板端部署或运行已验证。

## 1. 环境变量

```bash
export IBMW_SOURCE_DIR="<ibmw-source-dir>"
export OHOS_SDK_NATIVE_ROOT="<ohos-sdk-native-root>"
export ROS2_TARGET_PREFIX="<ohos-ros2-install-prefix>"
export BUILD_DIR="${TMPDIR:-/tmp}/ibmw-ohos-humble-build"
export HOST_PYTHON="<host-python3>"
```

`$ROS2_TARGET_PREFIX` 必须是 OHOS/AArch64 目标依赖前缀，不能用主机
`/opt/ros/...` 代替。更换源码目录、生成器、SDK 或目标前缀时应使用新的
`$BUILD_DIR`。

## 2. 前缀检查

目标前缀至少应提供所选构建所需的 FastCDR、FastDDS/FastRTPS 与 ROS2
接口包。rosidl 路径还需要 `ament_cmake`、`rosidl_default_generators`、
`builtin_interfaces` 等生成依赖。

```bash
bash "$IBMW_SOURCE_DIR/scripts/deps/check_target_ros2_prefix.sh" \
  "$ROS2_TARGET_PREFIX" fastdds types rosidl
```

如果需要从源码补充接口包，可先按仓库 helper 的参数准备 overlay，再把
overlay 与 underlay 组成冒号分隔的目标前缀列表。

## 3. 推荐构建入口

```bash
IBMW_OHOS_ROS2_PREFIX_LIST="$ROS2_TARGET_PREFIX" \
IBMW_OHOS_ROS2_PYTHONPATH="$ROS2_TARGET_PREFIX/lib/python3/site-packages" \
IBMW_OHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT" \
IBMW_OHOS_PYTHON_EXECUTABLE="$HOST_PYTHON" \
IBMW_OHOS_OPENSSL_ROOT_DIR="$OHOS_SDK_NATIVE_ROOT/sysroot/usr" \
IBMW_OHOS_OPENSSL_INCLUDE_DIR="$OHOS_SDK_NATIVE_ROOT/sysroot/usr/include" \
IBMW_OHOS_OPENSSL_SSL_LIBRARY="$OHOS_SDK_NATIVE_ROOT/sysroot/usr/lib/libssl.so" \
IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY="$OHOS_SDK_NATIVE_ROOT/sysroot/usr/lib/libcrypto.so" \
bash "$IBMW_SOURCE_DIR/scripts/check_ohos_ros2_rosidl_compile.sh" "$BUILD_DIR"
```

若构建环境需要缓存的第三方源码，可额外设置：

```bash
export IBMW_THIRD_PARTY_LOCAL_SOURCE_ROOT="<cached-source-root>"
```

不要让缓存中的另一套 FastDDS/FastCDR 覆盖目标 ROS2 前缀所要求的 ABI。
ROS2 Humble 目标前缀与本机较新 FastDDS 版本可能不兼容。

## 4. 主机与目标隔离

手工配置时清理主机包搜索变量，仅保留显式目标路径：

```bash
env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
  AMENT_PREFIX_PATH="$ROS2_TARGET_PREFIX" \
  PYTHONPATH="$ROS2_TARGET_PREFIX/lib/python3/site-packages" \
  cmake -S "$IBMW_SOURCE_DIR" -B "$BUILD_DIR" \
    -DOHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT" \
    -DIBMW_TARGET_ROS2_DEPS_PREFIX="$ROS2_TARGET_PREFIX"
```

主机 Python 可用于代码生成，但其头文件或库只有在构建逻辑明确要求主机工具
时才能传入；不得把主机 ROS2/FastDDS 库链接进目标 ELF。

## 5. 产物审计

```bash
python3 -m json.tool \
  "$BUILD_DIR/logs/route_b_target_artifact_audit.json"
file "$BUILD_DIR/<target-artifact>"
readelf -h "$BUILD_DIR/<target-artifact>"
readelf -d "$BUILD_DIR/<target-artifact>"
```

验收要点：

- 审计状态成功，目标架构为 AArch64；
- 依赖来自 `$OHOS_SDK_NATIVE_ROOT` 或 `$ROS2_TARGET_PREFIX`；
- `DT_NEEDED` 不含绝对主机路径；
- 安装后的 CMake 配置不引用 `$BUILD_DIR`；
- 全新 external consumer 能只依赖安装前缀完成配置和构建。

## 6. 能力边界

- DDS-only ROS2 互操作不要求 IBMW 运行时代码链接 rcl/rclcpp。
- Action 仅有实验性适配范围，不是通用自动迁移能力。
- 编译与 ELF 审计不能替代设备上的加载、发现、权限、网络、共享内存与业务
  流程验证。

更多通用原则见[交叉编译指南](cross-compilation.md)与
[支持矩阵](../reference/support-matrix.md)。
