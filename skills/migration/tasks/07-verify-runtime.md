Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 07 — Verify Runtime

Create one log per verified lane. PASS markers are strings inside those logs,
not required filenames:

```text
reports/runtime_logs/dds_ros2_pubsub.log       # contains DDS_ROS2_PUBSUB_PASS
reports/runtime_logs/dds_ros2_service_client.log # contains DDS_ROS2_SERVICE_PASS
reports/runtime_logs/dds_ros2_service_server.log
reports/runtime_logs/json_local_pubsub.log       # contains JSON_LOCAL_PUBSUB_PASS
reports/runtime_logs/json_local_service.log      # contains JSON_LOCAL_SERVICE_PASS
```

PASS marker grep:

```bash
rg -n 'DDS_ROS2_(PUBSUB|SERVICE)_PASS|JSON_LOCAL_(PUBSUB|SERVICE)_PASS' reports/runtime_logs/*.log
```

rcl-free audit before PASS:

```bash
rg -n 'rclcpp|rcl_|RCLCPP_|create_(publisher|subscription|service|client|wall_timer)|spin\(' dds_ros2_application adapters
```

Expected: PASS marker command finds intended markers; rcl-free audit has no
DDS/ROS2 application runtime hits.
