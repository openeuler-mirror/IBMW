Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# SDK Profiles and Claim Ladder

Use one SDK profile per target architecture and capability tier. A developer's
module set determines the required profile.

## Full development SDK boundary

The desired future profile is a full development SDK that supports many teams and
feature components from one per-architecture install prefix. That SDK must satisfy
two conditions at the same time:

1. capability coverage: local/minimal, JSON, DDS, ROS2-compatible DDS, generated
   `.msg` / `.srv` / `.action`, helper CMake modules, and host-side generators;
2. dependency isolation: a consumer app only links and deploys the components it
   uses, so a local app is not forced to carry FastDDS/ROS2 dependencies just
   because the SDK contains them.

Use componentized exports and audit each application's dependency closure. When
that cannot be demonstrated, build the SDK for the needed capability set.

| Profile | Typical flags/provider | Ships | Supports developing | Proven claim |
|---------|------------------------|-------|---------------------|--------------|
| `host_full` | host build with ROS2/DDS enabled | host libs, DDS/ROS2 headers, helper tools | all host modules: JSON/local, DDS/ROS2, generated projects | host runtime only |
| `aarch64_dds_ros2` | cross toolchain plus target DDS/Fast-CDR/ROS2 prefixes | target libs and headers | DDS/ROS2 and JSON/local modules for AArch64 | cross audit unless board log exists |
| `ohos_minimal` | `IBMW_BUILD_DDS_EXTENSION=OFF`, `IBMW_BUILD_WITH_ROS2=OFF`, `IBMW_TARGET_ROS2_TIER=none` | target headers/CMake exports; no DDS/ROS2 transport `.so` | minimal/local generated modules only | OHOS board hello-world seed |
| `ohos_dds_ros2` | OHOS toolchain plus target DDS/Fast-CDR/ROS2 provider | target DDS/ROS2 libs/headers and deps | DDS/ROS2 and generated modules on OHOS | OHOS board runtime for the verified Fibonacci DDS action seed; otherwise cross audit until case-specific board log exists |

Rules:

- Do not develop DDS/ROS2 modules against `ohos_minimal`.
- For one-place-build/many-developers, publish the exact profile name, target
  architecture, dependency prefix closure, and CMake toolchain file with the SDK.
- Do not use a trimmed/minimal SDK as a general multi-component development SDK.
- Do not assume a full-flag SDK is safe for all consumers until each consumer's
  actual target ELF closure is audited for unused/unrequested dependencies.
- Customer module repositories consume the SDK with `find_package(ibmw)` and do
  not rebuild the IBMW source tree.
- Host generator tools may run from the host/container while produced C++ is
  compiled with the target SDK/toolchain.
- Every runtime claim must name its tier: `(host runtime)`, `(OHOS board
  runtime)`, or `(cross audit only)`.
- DDS/ROS2 OHOS board runtime is **NOT VERIFIED** without a DDS/ROS2
  `/data/<case>` target run log.
