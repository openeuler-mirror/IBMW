Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 03 — Route B / OHOS SDK Rules

Goal: keep SDK-based OHOS cross builds target-safe and reproducible.

First choose the SDK profile:

- `ohos_minimal`: no DDS/ROS2 transport `.so`; proves only minimal/local module
  development and hello-world style board runtime when a `/data` log exists.
- `ohos_dds_ros2`: target DDS/ROS2-capable SDK; supports DDS/ROS2 module
  development only when target DDS/Fast-CDR/ROS2 dependency prefixes are present
  and the Route B audit passes.

Rules:

1. Build inside or against the Route B container/cross environment first.
2. Record execution location: `inside_route_b_container` or
   `outside_route_b_container`.
3. Record container-visible SDK, sysroot, target ROS2 prefix, and build paths.
4. Prefer Route B target ROS2 FastDDS/Fast-CDR. Do not use a bundled/newer
   Fast-CDR unless there is a reviewed exception.
5. Run target artifact audit and keep the JSON evidence.
6. For DDS/ROS2 SDK claims, deploy at least one DDS/ROS2 executable pair to the
   board and keep `/data/<case>` server/client logs.

Required audit:

```text
<route_b_build>/logs/route_b_target_artifact_audit.json
```

Producer: `scripts/check_ohos_route_b_link_boundary.sh` directly, or the Route B
compile wrapper that invokes it.

Expected result:

- `status=pass`
- `expected_arch=aarch64`
- all audited ELF artifacts report `arch=aarch64`
- no audited target ELF contains an absolute `DT_NEEDED` entry; provider libs
  must resolve by SONAME such as `libfastrtps.so.2.6` / `libfastcdr.so.1`, not a
  host/container path

Stop rules:

- SDK configure/build PASS is not board runtime validation.
- Container shell location is not target evidence.
- Fast-CDR/FastDDS version-source conflicts block SDK readiness until reviewed.
- DDS/ROS2 OHOS board runtime is NOT VERIFIED for a new SDK/profile without a
  matching DDS/ROS2 `/data/<case>` target run log, even if cross build/link/audit
  passes.
