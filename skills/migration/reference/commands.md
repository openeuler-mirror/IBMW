Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Reference Commands

Inventory:

```bash
rg -n 'create_(publisher|subscription|service|client|wall_timer)|declare_parameter|Action|action' native
```

Import ROS2 interface metadata/artifacts:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py import \
  --ros2-prefix /opt/ros/humble \
  --ros2-package <pkg> \
  --ros2-type <msg/String|srv/AddTwoInts|action/Fibonacci> \
  --artifact-only \
  --target dds_ros2 \
  --format json \
  --output-dir <workdir>/import_artifacts/<pkg>_<type>_dds
```

Generate an ecosystem-role project from YAML:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py <path/to/app.ibmw.yaml> \
  --output-dir <path/to/generated_project> \
  --force
```

rcl-free DDS/ROS2 application audit:

```bash
rg -n 'rclcpp|rcl_|RCLCPP_|create_(publisher|subscription|service|client|wall_timer)|spin\(' dds_ros2_application adapters
```

Adapter call audit:

```bash
rg -n 'adapter|Adapter|handle_|process_' dds_ros2_application json_local_application adapters
```

PASS marker grep:

```bash
rg -n 'DDS_ROS2_(PUBSUB|SERVICE)_PASS|JSON_LOCAL_(PUBSUB|SERVICE)_PASS' reports/runtime_logs/*.log
```

Light configure/build template:

```bash
set +u
source /opt/ros/humble/setup.bash
set -u
cmake -S . -B build -DCMAKE_PREFIX_PATH=<ibmw_install>\;/opt/ros/humble
cmake --build build --target <target> -j2
```

Runtime log capture pattern:

```bash
mkdir -p reports/runtime_logs
timeout 8s ./<app> build/cfg/<app>.yaml > reports/runtime_logs/<lane>.log 2>&1 || true
rg -n '<EXPECTED_PASS_TOKEN>' reports/runtime_logs/<lane>.log
```

OHOS Route B cross-compile / audit gate:

```bash
# First record where this command is executed.
# Use one of:
#   execution_location=outside_route_b_container
#   execution_location=inside_route_b_container
# If inside a Route B container, paths below are container-visible paths.
# FastDDS/Fast-CDR should resolve from the target ROS2 prefix, not from a bundled
# newer fallback, unless the exception is explicitly reviewed and recorded.

# Optional: prepare/validate ROS2 target overlay first.
bash scripts/deps/prepare_ohos_ros2_rosidl_overlay.sh --check-only

# Route B compile-only gate and target artifact audit.
# Use a fresh, unique <route_b_build>. Never switch generators in an existing
# cache. On reuse, first require CMAKE_HOME_DIRECTORY to exactly match the repo.
IBMW_OHOS_ROS2_PREFIX_LIST=<overlay-prefix>:<ohos-ros2-underlay-prefix> \
IBMW_OHOS_ROS2_PYTHONPATH=<ohos-ros2-underlay-prefix>/lib/python3.12/site-packages \
IBMW_OHOS_SDK_NATIVE_ROOT=<ohos-sdk-native-root> \
IBMW_OHOS_PYTHON_EXECUTABLE=<host-python> \
IBMW_OHOS_PYTHON_INCLUDE_DIR=<host-python-include-dir> \
IBMW_OHOS_PYTHON_LIBRARY=<host-python-library> \
IBMW_OHOS_OPENSSL_ROOT_DIR=<ohos-sdk-native-root>/sysroot/usr \
IBMW_OHOS_OPENSSL_INCLUDE_DIR=<ohos-sdk-native-root>/sysroot/usr/include \
IBMW_OHOS_OPENSSL_SSL_LIBRARY=<ohos-sdk-native-root>/sysroot/usr/lib/libssl.so \
IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY=<ohos-sdk-native-root>/sysroot/usr/lib/libcrypto.so \
IBMW_THIRD_PARTY_LOCAL_SOURCE_ROOT=<cached-fetchcontent-root> \
bash scripts/check_ohos_ros2_rosidl_compile.sh <route_b_build>

# The helper clears CMAKE_PREFIX_PATH, COLCON_PREFIX_PATH, and ROS_PACKAGE_PATH
# for configure/build while preserving explicit target AMENT_PREFIX_PATH and
# PYTHONPATH. Record the source directory and generator after configure.
rg -n '^(CMAKE_HOME_DIRECTORY|CMAKE_GENERATOR):' <route_b_build>/CMakeCache.txt

# Audit evidence. Expected: status=pass, expected_arch=aarch64, artifacts arch=aarch64.
python3 -m json.tool <route_b_build>/logs/route_b_target_artifact_audit.json

# Dependency-source evidence. Expected: fastcdr/fastrtps or FastDDS resolve under
# the target ROS2 prefix/underlay/overlay used for Route B.
rg -n 'fastcdr|fastrtps|FastDDS|Fast-CDR|CMAKE_PREFIX_PATH|IBMW_OHOS_ROS2_PREFIX_LIST' \
  reports/route_b_ohos.md <route_b_build>/CMakeCache.txt

# Record target provider realpaths and SONAMEs, then confirm the FastRTPS
# FastCDR DT_NEEDED exactly matches the selected FastCDR SONAME.
fastcdr_real=$(realpath <target-prefix>/lib/libfastcdr.so)
fastrtps_real=$(realpath <target-prefix>/lib/libfastrtps.so)
printf 'fastcdr_real=%s\nfastrtps_real=%s\n' "$fastcdr_real" "$fastrtps_real"
fastcdr_soname=$(readelf -d "$fastcdr_real" | rg -o 'SONAME.*\[[^]]+\]' | rg -o '\[[^]]+\]' | tr -d '[]')
fastrtps_soname=$(readelf -d "$fastrtps_real" | rg -o 'SONAME.*\[[^]]+\]' | rg -o '\[[^]]+\]' | tr -d '[]')
printf 'fastcdr_soname=%s\nfastrtps_soname=%s\n' "$fastcdr_soname" "$fastrtps_soname"
readelf -d "$fastrtps_real" | rg -F "Shared library: [$fastcdr_soname]"

# Final install-consumer gate: both directories must be clean/new.
cmake --install <route_b_build> --prefix <clean-ibmw-install-prefix>
env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
  AMENT_PREFIX_PATH=<target-prefix-list> \
  PYTHONPATH=<target-ros2-pythonpath> \
  cmake -S <external-consumer-source> -B <clean-external-consumer-build> \
    -DCMAKE_PREFIX_PATH=<clean-ibmw-install-prefix>
env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
  AMENT_PREFIX_PATH=<target-prefix-list> \
  PYTHONPATH=<target-ros2-pythonpath> \
  cmake --build <clean-external-consumer-build>
# Reject build-tree references in installed config/link metadata and reject all
# absolute DT_NEEDED entries in produced target ELFs.
rg -n '<route_b_build>' <clean-ibmw-install-prefix>/lib/cmake/ibmw \
  <clean-external-consumer-build> && exit 1 || true
readelf -d <consumer-target-elf> | rg 'NEEDED.*\[/' && exit 1 || true
```

Route B container-inside note:

- It is valid for a customer/agent to run the command directly inside the Route B
  build container.
- Still record the container image/name, mounted repo path, SDK/sysroot path,
  target ROS2 prefix list, and route_b build directory in `reports/route_b_ohos.md`.
- Do not use a container's host-tool `/opt/ros/humble` as target runtime proof.
  The target audit JSON must prove AArch64 artifacts and no forbidden host-prefix
  leak.
- Prefer the container/target ROS2 prefix's FastDDS/Fast-CDR. Avoid letting
  `_deps`, FetchContent, or an IBMW-bundled newer Fast-CDR override the ROS2
  version; mixed exports are a known conflict source.
