Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Backend Status Boundaries

- The DDS/ROS2-compatible application role is the runtime target for ROS2 interop.
- JSON pub/sub and JSON RPC are valid for local application practice and smoke verification.
- OHOS delivery requires Route B cross-compile plus target artifact audit. Host
  JSON smoke alone is only functional evidence, not deployment readiness.
- The Route B command may run outside a build container or directly inside a
  customer Route B container. In both modes, target artifact audit is the
  authority; execution location alone does not prove deployability.
- Route B should resolve FastDDS/Fast-CDR from the ROS2 target prefix whenever
  available. Do not mix in an unreviewed bundled/newer Fast-CDR provider.
- Local JSON smoke must not be generalized to another backend or deployment.
- ROS 2 actions are not auto-migrated; action business semantics require manual
  design as topics/services or an explicit deferral.
