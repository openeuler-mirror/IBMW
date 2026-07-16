Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# SDK Commands

Host/native SDK install seed:

```bash
cmake --build <ibmw-build> -j$(nproc)
cmake --install <ibmw-build> --prefix <sdk>
```

External generated project consumption:

```bash
cmake -S <generated-project> -B <consumer-build> \
  -DCMAKE_PREFIX_PATH="<sdk>[;<target-deps>]"
cmake --build <consumer-build> -j$(nproc)
```

Cross consumer pattern:

```bash
cmake -S <generated-project> -B <consumer-build> \
  -DCMAKE_TOOLCHAIN_FILE=<toolchain-file> \
  -DCMAKE_PREFIX_PATH="<sdk>[;<target-deps>]" \
  -DCMAKE_FIND_ROOT_PATH="<sdk>[;<target-deps>]"
cmake --build <consumer-build> -j$(nproc)
```

Minimal OHOS SDK generation seed:

```bash
cmake -S "$IBMW_SOURCE_DIR" -B "$OHOS_SDK_BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ohos-arm64-v8a.cmake \
  -DOHOS_SDK_NATIVE_ROOT=<ohos-sdk-native-root> \
  -DIBMW_BUILD_PROTOCOLS=OFF \
  -DIBMW_BUILD_DDS_EXTENSION=OFF \
  -DIBMW_BUILD_WITH_ROS2=OFF \
  -DIBMW_BUILD_ROS2_TYPES=OFF \
  -DIBMW_TARGET_ROS2_TIER=none
cmake --build "$OHOS_SDK_BUILD_DIR" -j2
cmake --install "$OHOS_SDK_BUILD_DIR" --prefix "$OHOS_SDK_PREFIX"
```

Minimal OHOS SDK consumer/run seed:

```bash
cmake -S "$IBMW_SOURCE_DIR/src/samples/cpp/quickstart/quickstart_gen" \
  -B "$OHOS_CONSUMER_BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ohos-arm64-v8a.cmake \
  -DOHOS_SDK_NATIVE_ROOT=<ohos-sdk-native-root> \
  -DCMAKE_PREFIX_PATH="$OHOS_SDK_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$OHOS_SDK_PREFIX"
cmake --build "$OHOS_CONSUMER_BUILD_DIR" -j2
scp "$OHOS_CONSUMER_BUILD_DIR/helloworld_app" <target>:/data/<case>/
```

Host validation pattern:

```bash
cmake -S "$IBMW_SOURCE_DIR/src/samples/cpp/quickstart/quickstart_gen" \
  -B "$HOST_CONSUMER_BUILD_DIR" \
  -DCMAKE_PREFIX_PATH="$HOST_SDK_PREFIX;/opt/ros/humble"
cmake --build "$HOST_CONSUMER_BUILD_DIR" -j2
timeout 3s env LD_LIBRARY_PATH="$HOST_SDK_PREFIX/lib:/opt/ros/humble/lib:$LD_LIBRARY_PATH" \
  "$HOST_CONSUMER_BUILD_DIR/helloworld_app" \
  "$HOST_CONSUMER_BUILD_DIR/cfg/helloworld_app.yaml"
```

Route B audit evidence:

```bash
python3 -m json.tool <route_b_build>/logs/route_b_target_artifact_audit.json
```
