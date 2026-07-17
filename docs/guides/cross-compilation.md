# Cross-compilation Guide

IBMW cross builds separate host tools from target libraries. Use an explicit
toolchain/sysroot and an explicit target dependency prefix; never allow a host
ROS2 installation to satisfy target package lookup.

## Variables

```bash
export IBMW_SOURCE_DIR="<ibmw-source-dir>"
export BUILD_DIR="${TMPDIR:-/tmp}/ibmw-cross-build"
export TARGET_SYSROOT="<target-sysroot>"
export ROS2_TARGET_PREFIX="<target-ros2-install-prefix>"
export TOOLCHAIN_FILE="$IBMW_SOURCE_DIR/cmake/toolchains/aarch64-linux-gnu.cmake"
```

Use a new build directory when changing source trees, generators, toolchains, or
target prefixes.

## AArch64 Linux without ROS2 types

```bash
cmake -S "$IBMW_SOURCE_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_SYSROOT="$TARGET_SYSROOT" \
  -DIBMW_BUILD_PROTOCOLS=OFF \
  -DIBMW_BUILD_WITH_ROS2=OFF \
  -DIBMW_BUILD_ROS2_TYPES=OFF \
  -DIBMW_BUILD_EXAMPLES=ON
cmake --build "$BUILD_DIR" -j"$(nproc)"
```

## AArch64 Linux with DDS/ROS2 interface packages

The target prefix must contain target-architecture FastCDR, FastDDS/FastRTPS,
and the ROS2 interface packages required by the selected target. Validate it
before configure:

```bash
bash "$IBMW_SOURCE_DIR/scripts/deps/check_target_ros2_prefix.sh" \
  "$ROS2_TARGET_PREFIX" fastdds types
```

For generated rosidl work, use the stricter lane:

```bash
bash "$IBMW_SOURCE_DIR/scripts/deps/check_target_ros2_prefix.sh" \
  "$ROS2_TARGET_PREFIX" fastdds types rosidl
```

Then configure with host package paths removed and target paths explicit:

```bash
env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
  AMENT_PREFIX_PATH="$ROS2_TARGET_PREFIX" \
  cmake -S "$IBMW_SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_SYSROOT="$TARGET_SYSROOT" \
    -DIBMW_TARGET_ROS2_DEPS_PREFIX="$ROS2_TARGET_PREFIX" \
    -DIBMW_BUILD_WITH_ROS2=OFF \
    -DIBMW_BUILD_ROS2_TYPES=ON \
    -DIBMW_BUILD_DDS_EXTENSION=ON
cmake --build "$BUILD_DIR" -j"$(nproc)"
```

## OHOS ARM64

```bash
export OHOS_SDK_NATIVE_ROOT="<ohos-sdk-native-root>"
export BUILD_DIR="${TMPDIR:-/tmp}/ibmw-ohos-build"
export ROS2_TARGET_PREFIX="<ohos-ros2-install-prefix>"
```

For the DDS-only lane:

```bash
cmake -S "$IBMW_SOURCE_DIR" -B "$BUILD_DIR" \
  -DOHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT" \
  -DIBMW_BUILD_WITH_ROS2=OFF \
  -DIBMW_BUILD_ROS2_TYPES=OFF \
  -DIBMW_BUILD_DDS_EXTENSION=ON
cmake --build "$BUILD_DIR" -j"$(nproc)"
```

For the ROS2 interface/rosidl lane, provide the target prefix and any required
host Python explicitly. Prefer the repository helper because it isolates host
package variables and performs target artifact checks:

```bash
IBMW_OHOS_ROS2_PREFIX_LIST="$ROS2_TARGET_PREFIX" \
IBMW_OHOS_SDK_NATIVE_ROOT="$OHOS_SDK_NATIVE_ROOT" \
bash "$IBMW_SOURCE_DIR/scripts/check_ohos_ros2_rosidl_compile.sh" "$BUILD_DIR"
```

## Verification

Inspect target artifacts rather than relying on configure success:

```bash
file "$BUILD_DIR/<target-artifact>"
readelf -h "$BUILD_DIR/<target-artifact>"
readelf -d "$BUILD_DIR/<target-artifact>"
```

Confirm that:

- ELF architecture matches the target.
- dynamic dependencies do not contain absolute host paths;
- CMake cache/package metadata resolves target libraries from the sysroot or
  `$ROS2_TARGET_PREFIX`;
- installed package metadata does not reference `$BUILD_DIR`;
- a clean external consumer can configure against the install prefix.

Cross compilation proves target artifact construction. Deployment, discovery,
permissions, shared-memory setup, and board runtime require separate target-side
validation. See the [Support Matrix](../reference/support-matrix.md).
