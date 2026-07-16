Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 08 — Route B / OHOS Cross-Compile Gate

Goal: prove the migration is not host-only when the target is OHOS.

Host ecosystem-role runtime smoke proves business behavior. It does **not** prove
OHOS deployment readiness. For OHOS, collect Route B compile/audit evidence.

Steps:

1. Record execution location in `reports/migration_report.md`:
   `outside_route_b_container` or `inside_route_b_container`.
2. Record target inputs in `reports/migration_report.md`:
   OHOS SDK native root, ROS2 target prefix/underlay, overlay prefix, host Python,
   FastDDS/Fast-CDR provider, third-party source cache, and route_b build
   directory.
3. Use a fresh, unique final Route B build directory. Never change generators in
   an existing cache. If reusing a cache, verify `CMAKE_HOME_DIRECTORY` is exactly
   the IBMW repo root and record its `CMAKE_GENERATOR`; otherwise stop and choose
   a new directory. Do not delete a user's build tree.
4. Validate or prepare the target ROS2 overlay if used.
5. Run `scripts/check_ohos_ros2_rosidl_compile.sh <route_b_build>` with explicit
   OHOS environment variables from `reference/commands.md`.
   Configure and build with `CMAKE_PREFIX_PATH`, `COLCON_PREFIX_PATH`, and
   `ROS_PACKAGE_PATH` cleared; retain only the explicit target
   `AMENT_PREFIX_PATH` and `PYTHONPATH`.
6. Record the target FastRTPS and FastCDR library realpaths and SONAMEs. Verify
   the selected `libfastrtps` `DT_NEEDED` entry for FastCDR equals the selected
   `libfastcdr` SONAME.
7. Verify `<route_b_build>/logs/route_b_target_artifact_audit.json` exists and
   the Route B checker rejects absolute `DT_NEEDED` entries.
8. Install to a clean prefix, then configure and build a clean external
   `find_package(ibmw CONFIG REQUIRED)` consumer against that prefix. Verify the
   installed config resolves target ROS dependencies without build-tree paths.
9. Mark migration deployable only if the audit reports `status=pass`,
   `expected_arch=aarch64`, and all audited ELF artifacts are `aarch64`.
10. To upgrade from compile/audit to board runtime, deploy the target artifacts to
   `/data/<case>`, run the migrated executable(s), and keep server/client logs.

Execution-location warning:

- If an agent runs **inside** a Route B container, do not assume container paths
  are host paths. Record container image/name, workspace mount, SDK mount, and
  target prefix paths as seen inside the container.
- If an agent runs **outside** the container, record the wrapper command or env
  used to enter the Route B build image.
- In both cases, the audit JSON is the authority; shell location alone is not
  proof of target safety.

FastDDS / Fast-CDR version rule:

- Prefer the FastDDS/Fast-CDR packages from the ROS2 target environment used for
  Route B. In practice, build inside or against the ROS2-enabled cross/container
  environment first, then point CMake to that target ROS2 prefix.
- Do not silently fall back to IBMW-bundled, `_deps`, or FetchContent-downloaded
  newer Fast-CDR/FastDDS when the ROS2 target prefix already provides them.
  Mixing versions can create exported-target conflicts, duplicate CMake package
  definitions, ABI mismatch, or runtime symbol conflicts.
- If no ROS2-provided Fast-CDR/FastDDS exists, record the exception and treat the
  migration as not Route-B-ready until the version/source choice is reviewed.

Evidence:

- Command log: `reports/route_b_ohos.md`
- Audit JSON: `<route_b_build>/logs/route_b_target_artifact_audit.json`
- Board logs, when claimed: `/data/<case>/*.log` copied into the evidence bundle
- Execution location: `outside_route_b_container` or `inside_route_b_container`
- FastDDS/Fast-CDR provider: ROS2 target prefix, not bundled high-version fallback
- Final report line: `OHOS Route B: PASS` or `OHOS Route B: NOT VERIFIED`

Stop rules:

- Do not use `/opt/ros/humble` as a target runtime prefix.
- Do not treat a container's host-tool ROS prefix as the OHOS target runtime
  prefix. Container-inside agents must still distinguish host tools, target
  underlay, target overlay, sysroot, and build output.
- Do not resolve Fast-CDR/FastDDS from a newer bundled dependency if the target
  ROS2 prefix provides them.
- Do not reuse a cache from another source tree or change its generator.
- Do not accept absolute `DT_NEEDED`, mismatched FastRTPS/FastCDR SONAMEs,
  build-tree paths in installed configs, or an untested install-tree consumer.
- Do not claim board/runtime validation from this gate alone; runtime validation
  requires separate `/data/<case>` logs from the OHOS target.
- If target prefixes or OHOS SDK are unavailable, record `OHOS Route B: NOT
  VERIFIED` and keep JSON/local application status host-only.
