Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# IBMW Skills Index

Use this index to pick the correct skill entry point.

| Need | Entry point | Notes |
|------|-------------|-------|
| ROS2 application migration to DDS/ROS2 and JSON/local ecosystem roles | `migration/SKILL.md` | Tasks, templates, checklists, runtime evidence, and OHOS target audit boundaries. |
| IBMW SDK delivery and consumption | `sdk/SKILL.md` | Generate and consume install-prefix SDKs for host/cross/customer builds without rebuilding IBMW source each time. |
| OHOS DDS / ROS2 minimal static-message delivery | `ohos_dds_ros2/SKILL.md` | Cross-build, target closure audit, HDC deployment, bidirectional interoperability, and evidence-level rules. |
| Transport decoupling and red-line rules | `transport_decoupling.md` | Canonical R1-R12 red-line skill; use before touching transport/module generation paths. |
| Test discipline and verification procedure | `ibmw_testing.md` | Use before changing tests, gates, fixtures, or verification scripts. |
| Legacy ROS2 app migration link | `ros2_app_migration.md` | Compatibility redirect to `migration/SKILL.md`; do not add new content here. |

Organization rule:

- Prefer one canonical skill per workflow.
- Redirect old entry points instead of duplicating boundary claims.
- Put reusable commands/status boundaries in the workflow's `reference/` files.
