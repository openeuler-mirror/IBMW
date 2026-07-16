Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 03 — DDS/ROS2 Application Service

Goal: replace ROS service runtime with DDS-compatible RPC/service modules.

Steps:

1. Copy `templates/dds_ros2_service.ibmw.yaml` to
   `dds_ros2_application/service/<app>_dds_ros2_service.ibmw.yaml`.
2. Fill service type, service name, request/response mapping, and DDS driver.
3. Generate server/client modules as needed.
4. User-region code must call an adapter such as
   `adapters::<service>_handle(request)` and map response fields only.
5. Preserve validation, error handling, and return values from native callback.

Evidence:

- YAML: `dds_ros2_application/service/*_dds_ros2_service.ibmw.yaml`
- Runtime pass marker: `DDS_ROS2_SERVICE_PASS` inside
  `reports/runtime_logs/dds_ros2_service_client.log` or equivalent.
- Audit command:

```bash
rg -n 'rclcpp|rcl_|RCLCPP_|create_(service|client)|spin\(' dds_ros2_application adapters
```

Expected: no hits in DDS/ROS2 application runtime or adapters.
