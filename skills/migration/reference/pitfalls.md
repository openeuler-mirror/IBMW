Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Pitfalls

- **Dynamic ROS2 payload ceiling mistaken for interoperability proof.** Generic
  dynamic ROS2 type support currently declares a **1 MiB serialized type
  ceiling**. Discovery and endpoint matching may still succeed when a real
  `sensor_msgs/msg/Image` or `sensor_msgs/msg/PointCloud2` sample exceeds that
  capacity; the DDS write can then fail without any representative data reaching
  the peer. Measure or conservatively estimate the application's largest
  serialized sample, compare it with the registered type capacity, use an
  app-local type-support override when more capacity is required, and require an
  external observer to receive a representative maximum-size payload. Do not
  confuse this registered ceiling with an app serializer's independent field
  validation maximum or with the actual size of one sample.
- Rewriting business logic in generated user regions. Fix: move logic to
  `adapters/` and call it from user regions.
- Leaving `rclcpp` or `RCLCPP_*` in DDS/ROS2 application runtime. Fix: keep ROS2 only in
  native harness/peer code and rerun the rcl-free audit.
- Generalizing JSON local smoke to another backend. Fix: state the local-only
  boundary and validate the requested backend separately.
- Claiming OHOS readiness from host smoke. Fix: run Route B cross-compile and
  inspect `route_b_target_artifact_audit.json`, or mark OHOS not verified.
- Running inside a Route B container and forgetting host/target separation. Fix:
  record `inside_route_b_container`, container-visible mounts, SDK/sysroot, target
  prefix list, and audit JSON. The container shell is not itself target evidence.
- Letting a bundled or FetchContent Fast-CDR/FastDDS override the ROS2 target
  environment version. Fix: prefer the Route B ROS2 prefix provider; mixed newer
  versions can cause exported-target conflicts, ABI mismatch, or runtime symbol
  conflicts.
- Reusing a stale CMake cache or changing its generator. Fix: use a fresh,
  unique final build directory; before reuse require `CMAKE_HOME_DIRECTORY` to
  exactly equal the repo root and keep the recorded `CMAKE_GENERATOR` unchanged.
- Inheriting host package discovery variables. Fix: clear `CMAKE_PREFIX_PATH`,
  `COLCON_PREFIX_PATH`, and `ROS_PACKAGE_PATH`; set only explicit target
  `AMENT_PREFIX_PATH` and target metadata `PYTHONPATH` for configure/build.
- Checking FastDDS and FastCDR paths but not their ABI edge. Fix: record both
  target library realpaths and SONAMEs, then require `libfastrtps`'s FastCDR
  `DT_NEEDED` to equal the selected `libfastcdr` SONAME.
- Testing only the build tree. Fix: install into a clean prefix and build a clean
  external `find_package(ibmw)` consumer; reject absolute `DT_NEEDED` and any
  installed-config dependency resolution that refers to build-tree paths.
- Treating local JSON smoke as DDS or shared-memory evidence. Fix: run the
  backend-specific build and runtime checks.
- Claiming actions migrated automatically. Fix: manually model action semantics
  as topics/services or mark deferred with evidence.
- PASS marker without log. Fix: PASS markers are log strings, not filenames;
  write stdout/stderr to `reports/runtime_logs/*.log` and grep for the token.
