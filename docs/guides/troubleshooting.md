<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# Troubleshooting Index

Use exact diagnostics before changing source or weakening a gate.

## FastDDS or FastCDR not found

- Native host build: source `/opt/ros/humble/setup.bash` with `set +u` / `set -u`
  around it, and verify the expected FastDDS/FastCDR package versions.
- Cross build: use the target DDS source prefix from
  [cross-compilation.md](cross-compilation.md). Do not let host ROS2 libraries
  satisfy target checks.
- Generated CDR compile errors often indicate a FastCDR version-lane mismatch;
  regenerate for the selected prefix instead of patching generated methods.

## `GLIBCXX_3.4.30` not found

Conda may put an older `libstdc++.so.6` first. For affected ROS2/FastDDS tests:

```bash
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

Then rerun the exact failed binary. This is an environment correction, not proof
that an unrelated test passed.

## Stale CMake or install metadata

Remove the affected build directory or its `CMakeCache.txt`/`CMakeFiles`, then
configure again. After in-place install, stale root-level
`ibmw-config.cmake`/`ibmw-config-version.cmake` can interfere with generated
consumer projects; follow the build recipe in the top-level README.

For OHOS Route B, never reuse a cache whose `CMAKE_HOME_DIRECTORY` names another
checkout, and do not change the cached CMake generator in place. The compile
checker reports both values and leaves the old tree untouched; select a fresh
build directory instead. Also clear inherited `CMAKE_PREFIX_PATH`,
`COLCON_PREFIX_PATH`, and `ROS_PACKAGE_PATH` so host ROS2 discovery cannot satisfy
target dependencies.

If an installed consumer cannot resolve DDS provider targets, verify that the
target prefix contains the intended FastRTPS/FastCDR SONAME lane, that installed
configuration files contain no build-tree absolute paths, and that the rosidl
consumer receives `ibmw_target_fastcdr` and `ibmw_target_fastrtps`. Run:

```bash
bash scripts/tests/test_ibmw_installed_config_targets.sh
bash scripts/tests/test_target_ros2_prefix.sh
bash scripts/tests/test_ohos_route_b_link_boundary.sh
```

## DDS discovery, zero counts, or unstable p99

- Do not classify timeout exit as functional delivery.
- Require publisher/reader readiness and real send/receive markers.
- Run FastDDS SHM cases sequentially on separate domain IDs; parallel cases can
  interfere through discovery or shared-memory artifacts.
- A non-zero count proves delivery, not zero-copy or performance. Check
  `can_loan=true`, `loan_failed=0`, `publish_failed=0`, and `fallback=0`, then use
  a clean non-gcov Release build and pinned repeats.
- Record the exact build, host conditions, repetitions, and comparison method
  before making a p99 claim.

## Process cleanup on the shared host

Never use `pkill`, broad pattern kills, or `kill -9 -1`. List processes, verify
ownership and command identity, then terminate only exact PIDs started by the
current run. Prefer graceful TERM before KILL and verify no owned process remains.

## Public/internal include failures

User code should include `cpp/...` or an installed
`type_support/<transport>/...` adapter. An `api/`-prefixed include, an
`internal/typesupport/...` include, or an `ibmw::internal` dependency in a
generated package indicates the wrong include lane. See
[Compatibility Boundaries](../reference/compatibility.md).

## ROS2 and Action scope confusion

Separate these lanes before debugging:

- deprecated rcl/rclcpp extension behavior;
- DDS-only ROS2 pub/sub or service interop;
- experimental DDS-only Action adapters;
- host compile/link, cross compile, and target board runtime.

A PASS in one lane is not evidence for another. Use the interface import and
Action guides linked from the [documentation index](../README.md).
