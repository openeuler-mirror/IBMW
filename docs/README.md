<!-- Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame. -->

# IBMW Documentation

This index contains public documentation for people using, integrating, and
contributing to IBMW. Start with the user guide, then follow the section that
matches your task.

## Users

- [User Guide](guides/user-guide.md) ([中文](guides/user-guide.zh.md)) — build,
  configure, and run IBMW applications and examples.
- [Troubleshooting](guides/troubleshooting.md) — common build, runtime,
  dependency, discovery, and cleanup problems.
- [Cross-compilation](guides/cross-compilation.md) — AArch64 Linux and scoped
  OHOS target-prefix workflows.
- [OHOS Xiaoyi Humble Cross-build Guide](guides/ohos-xiaoyi-humble-cross-build-guide.zh.md)
  — Chinese compile-only guide for the Xiaoyi environment.
- [OHOS DDS / ROS 2 Minimal Interoperability Guide](guides/ohos-dds-ros2-minimal-interop.zh.md)
  — canonical Chinese guide for target closure, static message products, HDC deployment,
  evidence levels, and host/on-device ROS 2 CLI interoperability.
- [ROS2 Interface Import](guides/ros2-interface-import-user-guide.md) — import
  installed or source ROS2 interfaces with `ibmw-gen`.
- [Transport Benchmark](guides/transport-benchmark.md)
  ([中文](guides/transport-benchmark.zh.md)) — reproduce and interpret the
  transport latency comparison.

## Reference

- [Architecture Overview](architecture/overview.md) — runtime layers, dependency
  direction, package ABI, and generated-code boundaries.
- [API Reference](reference/api-reference.md)
  ([中文](reference/api-reference.zh.md)) — C++ SDK, C ABI, extension, package,
  and type-support interfaces.
- [Compatibility Boundaries](reference/compatibility.md) — supported API,
  dependency, ROS2, DDS, Action, and cross-build boundaries.
- [Support Matrix](reference/support-matrix.md) — concise status and scope for
  public capabilities.

## Contributors

- [Contributing](../CONTRIBUTING.md) — contribution policy and verification
  expectations.
- [Developer Guide](contributing/developer-guide.md)
  ([中文](contributing/developer-guide.zh.md)) — project structure, modules,
  extensions, types, transports, RPC, and testing.
- [Architecture Rules](contributing/architecture-rules.md) — enforceable
  dependency, ABI, generation, and zero-copy constraints.
- [Testing and Verification](contributing/testing.md) — change-specific checks
  and evidence expectations.
- [Security Policy](../SECURITY.md), [Support](../SUPPORT.md), and
  [Code of Conduct](../CODE_OF_CONDUCT.md).

## Ecosystem

- [Ecosystem Index](ecosystem/README.md) — public migration and SDK resources.
- [Migration Resources](ecosystem/migration/README.md) — guided ROS2-to-IBMW
  application migration assets.
- [SDK Resources](ecosystem/sdk/README.md) — SDK delivery tasks, references,
  and checklists.

## Experimental

- [ROS2 Action DDS-only Adapter Guide](experimental/ros2-action-dds-only-adapter-guide.md)
  — hand-adapted, experimental Action server path with explicit support limits.

Experimental documentation describes scoped interfaces that may change and
must not be read as a general compatibility guarantee.

## Releases

- [Changelog](../CHANGELOG.md)
- [0.1.0 release notes](releases/0.1.0.md)
