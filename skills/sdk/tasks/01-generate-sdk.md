Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 01 — Generate an IBMW SDK Prefix

Goal: create an install-tree SDK that customer projects can consume without the
IBMW source tree.

Steps:

1. Select SDK profile:
   - host/native SDK
   - AArch64 Linux SDK
   - OHOS/Route B SDK
2. For cross SDKs, configure IBMW with the target toolchain first. The SDK's
   libraries and executable must be target-architecture artifacts.
3. Build IBMW with install enabled and the required extensions.
4. Build required schema/codegen targets before install when DDS/schema targets
   are part of the SDK.
5. Install to a clean prefix; do not install into a stale build dir.
6. Ensure target third-party dependencies are co-located with the SDK or listed
   on the consumer `CMAKE_PREFIX_PATH`.
7. Record SDK manifest: IBMW version, target triple, toolchain file,
   FastDDS/Fast-CDR provider, extension set, and build command.

Evidence:

- `<sdk>/lib/cmake/ibmw/ibmw-config.cmake`
- `<sdk>/lib/cmake/ibmw/ibmw-targets.cmake`
- `<sdk>/cmake/IbmwHelpers.cmake`
- `<sdk>/bin/dds_idl_gen_rpc.py` when DDS IDL/RPC generation is included

Stop rules:

- Do not call a build tree an SDK for customer Route B use.
- Do not reuse a host SDK as a target SDK.
- Do not omit third-party target dependency prefixes from SDK documentation.
- Do not ship unreviewed FetchContent/bundled Fast-CDR over a ROS2-provided one.
