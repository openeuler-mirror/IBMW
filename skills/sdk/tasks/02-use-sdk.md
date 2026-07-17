Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 02 — Use the SDK from a Customer Project

Goal: build a generated or hand-written customer project against an installed
IBMW SDK prefix.

Steps:

1. Unpack or mount the SDK prefix in the build environment.
2. Ensure host tools are available: CMake, C++ compiler/toolchain, Python for
   generation workflows, `fastddsgen`/JRE when IDL generation is needed.
3. Configure the customer project with `CMAKE_PREFIX_PATH=<sdk>[;<deps>]`.
4. For cross builds, also pass `CMAKE_TOOLCHAIN_FILE=<toolchain>`.
5. Build, then audit target artifacts for architecture and forbidden host leaks.

Evidence:

- Customer configure log shows `find_package(ibmw)` resolved from the SDK.
- Build log has no IBMW source-tree dependency.
- Runtime bundle lists required `.so` files from the SDK.
- Cross artifacts have expected target architecture.
- If OHOS runtime is in scope, `/data/<case>` target run log shows startup PASS.

Stop rules:

- Do not require customer builds to clone/rebuild IBMW unless the SDK is missing.
- Do not point an OHOS cross consumer at a host SDK prefix.
- Do not use host `/opt/ros/humble` as a target runtime prefix in cross builds.
- Do not claim board runtime validation from configure/build alone.
