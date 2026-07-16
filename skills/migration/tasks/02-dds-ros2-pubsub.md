Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 02 — DDS/ROS2 Application Pub/Sub

Goal: replace ROS pub/sub runtime with DDS/ROS2-compatible IBMW modules.

Steps:

1. Copy `templates/dds_ros2_pubsub.ibmw.yaml` to
   `dds_ros2_application/pubsub/<app>_dds_ros2_pubsub.ibmw.yaml`.
2. Fill `messages`, topic names, and `dds_driver.ros2_compatible_mode: true`.
   QoS/depth is not fully modeled by this template; record it in
   `reports/inventory.md` and preserve or explicitly degrade it in the adapter
   / runtime report.
3. Generate modules using the project generator command for that YAML.
4. In generated user regions, call only `adapters/*` functions.
5. Keep native ROS2 publisher/subscriber only as peer smoke harness if needed.
6. For every dynamic or large type, estimate a conservative maximum serialized
   size from bounded fields and configuration, then measure representative
   production samples with the active serializer. Record actual serialized
   bytes, expected maximum, and safety margin in `reports/inventory.md`.
7. Compare that payload budget with the DDS type capacity registered for the
   topic. The generic dynamic ROS2 type support currently declares 1 MiB; that
   is a floor/default, not evidence that every Image or PointCloud2 fits. Keep
   the registered type ceiling distinct from serializer field-validation limits
   and from each sample's actual serialized size.
8. If the budget exceeds the generic capacity, provide an application-local
   type-support override for only the affected type. Do not raise the global
   default to accommodate one application. Record the host and target capacity,
   including the FastDDS ABI lane used by Route B.
9. Publish an actual representative maximum-size sample and require the intended
   external ROS2/DDS observer to receive and validate it. Discovery, topic-list
   output, or a small synthetic sample does not close this gate.

Evidence:

- YAML: `dds_ros2_application/pubsub/*_dds_ros2_pubsub.ibmw.yaml`
- Adapter: `adapters/*`
- Payload budget and registered-capacity evidence: `reports/inventory.md` and
  `reports/migration_report.md`
- Large-sample writer and external-observer receive logs:
  `reports/runtime_logs/*payload*.log`
- Audit command:

```bash
rg -n 'rclcpp|rcl_|RCLCPP_|create_(publisher|subscription)|spin\(' dds_ros2_application adapters
```

Expected: no hits in DDS/ROS2 application runtime or adapters.
