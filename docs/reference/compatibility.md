<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# Compatibility Boundaries

This page indexes stable constraints. It does not replace the detailed guides or
case-specific verification.

## Runtime and performance

- Loaned/shared-memory paths preserve ownership and zero-copy semantics. A
  fallback, extra copy, allocation, or executor handoff is a behavior/performance
  change and requires focused evidence.
- p99 acceptance uses an application-defined band and a pinned repeat protocol.
  Functional receive markers are necessary but do not establish p99.
- Benchmark observations are environment-specific. Preserve the command,
  hardware and software context, repetitions, and interpretation when using a
  result as a compatibility or performance decision.

## Package ABI and headers

- Package loading has one public entry, `IbmwPkgEntry`, with typed-slot driver
  injection. Slot IDs are stable; string driver names and dual-entry ABI behavior
  are not public contracts. See [API Reference](api-reference.md).
- Public user includes use `cpp/...` and the installed
  `type_support/<transport>/...` adapters. Generated code must not include
  `internal/typesupport/...` or depend on an `ibmw::internal` target. Public
  JSON transport and serialization APIs are `cpp/util/json_transport.h` and
  `cpp/util/json_serialization.h`.

## ROS2, DDS, Action, and OHOS

- Preferred ROS2 interoperability is DDS-only via FastDDS. Native builds may use
  a ROS2 prefix for FastDDS/FastCDR and rosidl metadata; this does not imply a
  runtime rcl/rclcpp dependency.
- The rcl-based ROS2 extension/preset is deprecated compatibility surface.
- ROS2 interface import must keep generated IBMW runtime rcl-free. See
  [ROS2 Interface Import](../guides/ros2-interface-import-user-guide.md).
- DDS-only Action is experimental and scoped to documented Humble/FastDDS paths.
  It is not generic cross-RMW, cross-distro, board-runtime, or productized Action
  generator support. See the
  [adapter guide](../experimental/ros2-action-dds-only-adapter-guide.md).
- AArch64/OHOS target builds use target prefixes and sysroots. Host ROS2 libraries
  must not satisfy target dependencies. OHOS evidence is compile-only or scoped
  Route B evidence unless a board-runtime result is explicitly recorded. See
  [Cross-compilation](../guides/cross-compilation.md) and the
  [OHOS guide](../guides/ohos-xiaoyi-humble-cross-build-guide.zh.md).
- The validated OHOS Route B rosidl lane preserves the ROS2 Humble FastRTPS 2.6
  and FastCDR 1 target ABI. It must not probe or link host/bundled FastDDS 3 into
  that lane. Installed consumers receive path-free
  `ibmw_target_fastcdr`/`ibmw_target_fastrtps` provider targets reconstructed by
  the installed package configuration.
- Dynamic DDS type registration capacity is an application boundary. The generic
  validation ceiling is not proof that the largest real serialized DDS/ROS2 application
  payload fits. Record a payload budget, apply an app-local ceiling where needed,
  and verify the largest representative sample with an external observer.

## Dependency versions

- FastCDR generated code is version-sensitive. The validated FastCDR 2.x /
  FastDDS 3.x path uses native generated CDR; legacy FastCDR v1 or unknown-version
  lanes use the compatibility transform. Do not combine generated artifacts from
  one lane with libraries from another.
- FastDDS request/reply interoperability relies on public `WriteParams`,
  `SampleInfo`, and sample identity behavior, but wire claims remain constrained
  to the tested ROS2 Humble + `rmw_fastrtps` combinations.

For recurring setup failures, use
[Troubleshooting](../guides/troubleshooting.md).

For a concise capability view, see the [Support Matrix](support-matrix.md).
